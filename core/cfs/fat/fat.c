/*
 * Copyright (c) 2012, Institute of Operating Systems and Computer Networks (TU Brunswick).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Author: Christoph Peltz <peltz@ibr.cs.tu-bs.de>
 */

/**
 * \addtogroup Drivers
 * @{
 *
 * \addtogroup fat_driver
 * @{
 */

/**
 * \file
 *      FAT driver implementation
 * \author
 *      Christoph Peltz <peltz@ibr.cs.tu-bs.de>
 */
 
#include "fat.h"

#define DEBUG 0
#if DEBUG
#include <stdio.h>
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LONG_NAME (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)

uint8_t sector_buffer[512];
uint32_t sector_buffer_addr = 0;
uint8_t sector_buffer_dirty = 0;

uint16_t cfs_readdir_offset = 0;

struct file_system {
	struct diskio_device_info *dev;
	struct FAT_Info info;
	uint32_t first_data_sector;
} mounted;

#define CLUSTER_TO_SECTOR(cluster_num) (((cluster_num - 2) * mounted.info.BPB_SecPerClus) + mounted.first_data_sector)
#define SECTOR_TO_CLUSTER(sector_num) (((sector_num - mounted.first_data_sector) / mounted.info.BPB_SecPerClus) + 2)

struct PathResolver {
	uint16_t start, end;
	const char *path;
	char name[11];
};

struct file fat_file_pool[FAT_FD_POOL_SIZE];
struct file_desc fat_fd_pool[FAT_FD_POOL_SIZE];

#ifdef FAT_COOPERATIVE
#include "fat_coop.h"
extern void coop_switch_sp();
extern uint8_t coop_step_allowed;
extern uint8_t next_step_type;
enum {
	READ = 1,
	WRITE,
	INTERNAL
};

extern QueueEntry queue[FAT_COOP_QUEUE_SIZE];
extern uint16_t queue_start, queue_len;
#endif

/* Declerations */
static uint8_t is_EOC( uint32_t fat_entry );
uint32_t get_free_cluster(uint32_t start_cluster);
static uint16_t _get_free_cluster_16();
static uint16_t _get_free_cluster_32();
static uint32_t find_nth_cluster( uint32_t start_cluster, uint32_t n );
static void reset_cluster_chain( struct dir_entry *dir_ent );
static void add_cluster_to_file( int fd );
void print_current_sector();
void print_dir_entry( struct dir_entry *dir_entry );
void get_fat_info( struct FAT_Info *info );
static uint32_t read_fat_entry( uint32_t cluster_num );
void write_fat_entry( uint32_t cluster_num, uint32_t value );
static void calc_fat_block( uint32_t cur_cluster, uint32_t *fat_sec_num, uint32_t *ent_offset );
static uint8_t _make_valid_name( const char *path, uint8_t start, uint8_t end, char *name );
static void pr_reset( struct PathResolver *rsolv );
static uint8_t pr_get_next_path_part( struct PathResolver *rsolv );
static uint8_t pr_is_current_path_part_a_file( struct PathResolver *rsolv );
static uint8_t fat_read_block( uint32_t sector_addr );
static uint8_t fat_next_block();
static uint8_t lookup( const char *name, struct dir_entry *dir_entry, uint32_t *dir_entry_sector, uint16_t *dir_entry_offset );
static uint8_t get_dir_entry( const char *path, struct dir_entry *dir_ent, uint32_t *dir_entry_sector, uint16_t *dir_entry_offset, uint8_t create );
static uint8_t add_directory_entry_to_current( struct dir_entry *dir_ent, uint32_t *dir_entry_sector, uint16_t *dir_entry_offset );
static void update_dir_entry( int fd );
static void remove_dir_entry( uint32_t dir_entry_sector, uint16_t dir_entry_offset );
static uint8_t load_next_sector_of_file( int fd, uint32_t clusters, uint8_t clus_offset, uint8_t write );
static void make_readable_entry( struct dir_entry *dir, struct cfs_dirent *dirent );
uint8_t is_a_power_of_2( uint32_t value );
uint32_t round_down_to_power_of_2( uint32_t value );
static uint8_t _is_file( struct dir_entry *dir_ent );
static uint8_t _cfs_flags_ok( int flags, struct dir_entry *dir_ent );

/*Cluster Chain Functions*/
static uint8_t is_EOC( uint32_t fat_entry ) {
    if( mounted.info.type == FAT16 ) {
        if( fat_entry >= 0xFFF8 ) {
            return 1;
        }

    } else if( mounted.info.type == FAT32 ) {
        if( (fat_entry & 0x0FFFFFFF) >= 0x0FFFFFF8 ) {
            return 1;
        }
    }
    return 0;
}

/**
 * \brief Looks through the FAT to find a free cluster.
 *
 * \TODO Check for end of FAT and no free clusters.
 * \return Returns the number of a free cluster.
 */
uint32_t get_free_cluster(uint32_t start_cluster) {
    uint32_t fat_sec_num = 0;
    uint32_t ent_offset = 0;
    uint16_t i = 0;

    calc_fat_block( start_cluster, &fat_sec_num, &ent_offset );

    do {
        fat_read_block( fat_sec_num );
        if( mounted.info.type == FAT16 ) {
            i = _get_free_cluster_16();
        } else if( mounted.info.type == FAT32 ) {
            i = _get_free_cluster_32();
        }
        fat_sec_num++;
    } while( i == 512 );

    ent_offset = ((fat_sec_num - 1) - mounted.info.BPB_RsvdSecCnt) * mounted.info.BPB_BytesPerSec + i;
    if( mounted.info.type == FAT16 ) {
        ent_offset /= 2;
    } else if( mounted.info.type == FAT32 ) {
        ent_offset /= 4;
    }

	PRINTF("\nfat.c: get_free_cluster(start_cluster = %lu) = %lu", start_cluster, ent_offset);
    return ent_offset;
}

static uint16_t _get_free_cluster_16() {
    uint16_t entry = 0;
    uint16_t i = 0;

    for( i = 0; i < 512; i += 2 ) {
        entry = (((uint16_t) sector_buffer[i]) << 8) + ((uint16_t) sector_buffer[i+1]);
        if( entry == 0 ) {
            return i;
        }
    }

    return 512;
}

static uint16_t _get_free_cluster_32() {
    uint32_t entry = 0;
    uint16_t i = 0;

    for( i = 0; i < 512; i += 4 ) {
        entry = (((uint32_t) sector_buffer[i+3]) << 24) + (((uint32_t) sector_buffer[i+2]) << 16) + (((uint32_t) sector_buffer[i+1]) << 8) + ((uint32_t) sector_buffer[i]);
        if( (entry & 0x0FFFFFFF) == 0 ) {
            return i;
        }
    }

    return 512;
}

static uint32_t find_nth_cluster( uint32_t start_cluster, uint32_t n ) {
    uint32_t cluster = start_cluster,
            i = 0;

    for( i = 0; i < n; i++ ) {
        cluster = read_fat_entry( cluster );
    }

	PRINTF("\nfat.c: find_nth_cluster( start_cluster = %lu, n = %lu ) = %lu", start_cluster, n, cluster);
    return cluster;
}

static void reset_cluster_chain( struct dir_entry *dir_ent ) {
    uint32_t cluster = (((uint32_t) dir_ent->DIR_FstClusHI) << 16) + dir_ent->DIR_FstClusLO;
    uint32_t next_cluster = read_fat_entry( cluster );

    while( !is_EOC( cluster ) && cluster >= 2 ) {
        write_fat_entry( cluster, 0L );
        cluster = next_cluster;
        next_cluster = read_fat_entry( cluster );
    }

    write_fat_entry( cluster, 0L );
}

static void add_cluster_to_file( int fd ) {
    uint32_t free_cluster = get_free_cluster( 0 );
    uint32_t cluster = fat_file_pool[fd].nth_cluster;
    uint32_t n = cluster;
	PRINTF("\nfat.c: add_cluster_to_file( fd = %d ) = void", fd);

    if( fat_file_pool[fd].cluster == 0 ) {
        write_fat_entry( free_cluster, EOC );
        fat_file_pool[fd].dir_entry.DIR_FstClusHI = (uint16_t) (free_cluster >> 16);
        fat_file_pool[fd].dir_entry.DIR_FstClusLO = (uint16_t) (free_cluster);

        update_dir_entry( fd );

        fat_file_pool[fd].cluster = free_cluster;
        fat_file_pool[fd].n = 0;
        fat_file_pool[fd].nth_cluster = free_cluster;

		PRINTF("\n\tfat.c: File was empty, now has first cluster %lu added to Chain", free_cluster);
        return;
    }

    while( !is_EOC( n ) ) {
        cluster = n;
        n = read_fat_entry( cluster );
        fat_file_pool[fd].n++;
    }

    write_fat_entry( cluster, free_cluster );
    write_fat_entry( free_cluster, EOC );
    fat_file_pool[fd].nth_cluster = free_cluster;
	PRINTF("\n\tfat.c: File was NOT empty, now has cluster %lu as %lu. cluster to Chain", free_cluster, fat_file_pool[fd].n);
}

/*Debug Functions*/
void print_current_sector() {
    uint16_t i = 0;

    for(i = 0; i < 512; i++) {
        printf("%02x", sector_buffer[i]);
        if( ((i+1) % 2) == 0 ) {
            printf(" ");
        }
        if( ((i+1) % 32) == 0 ) {
            printf("\n");
        }
    }
}

void print_cluster_chain( int fd ) {
	uint32_t cluster = fat_file_pool[fd].cluster;
	printf("\nClusterchain for fd = %d\n", fd);
	do{
		printf("%lu ->", cluster);
	}while(!is_EOC(cluster = read_fat_entry(cluster)));
	printf("%lu\n", cluster);
}

void print_file_info( int fd ) {
	printf("\nFile Info for fd = %d", fd);
	printf("\n\toffset = %lu", fat_fd_pool[fd].offset);
	printf("\n\tflags = %x", fat_fd_pool[fd].flags);
	printf("\n\tfile = %p", fat_fd_pool[fd].file);
	printf("\n\t\tcluster = %lu", fat_file_pool[fd].cluster);
	printf("\n\t\tdir_entry_sector = %lu", fat_file_pool[fd].dir_entry_sector);
	printf("\n\t\tdir_entry_offset = %u", fat_file_pool[fd].dir_entry_offset);
	printf("\n\t\tnth_cluster = %lu", fat_file_pool[fd].nth_cluster);
	printf("\n\t\tn = %lu", fat_file_pool[fd].n);
	printf("\n\t\tdir_entry");
    printf("\n\t\t\tDIR_Name = %c%c%c%c%c%c%c%c%c%c%c", fat_file_pool[fd].dir_entry.DIR_Name[0], fat_file_pool[fd].dir_entry.DIR_Name[1],fat_file_pool[fd].dir_entry.DIR_Name[2],fat_file_pool[fd].dir_entry.DIR_Name[3],fat_file_pool[fd].dir_entry.DIR_Name[4],fat_file_pool[fd].dir_entry.DIR_Name[5],fat_file_pool[fd].dir_entry.DIR_Name[6],fat_file_pool[fd].dir_entry.DIR_Name[7],fat_file_pool[fd].dir_entry.DIR_Name[8],fat_file_pool[fd].dir_entry.DIR_Name[9],fat_file_pool[fd].dir_entry.DIR_Name[10]);
    printf("\n\t\t\tDIR_Attr = %x", fat_file_pool[fd].dir_entry.DIR_Attr);
    printf("\n\t\t\tDIR_NTRes = %x", fat_file_pool[fd].dir_entry.DIR_NTRes);
    printf("\n\t\t\tCrtTimeTenth = %x", fat_file_pool[fd].dir_entry.CrtTimeTenth);
    printf("\n\t\t\tDIR_CrtTime = %x", fat_file_pool[fd].dir_entry.DIR_CrtTime);
    printf("\n\t\t\tDIR_CrtDate = %x", fat_file_pool[fd].dir_entry.DIR_CrtDate);
    printf("\n\t\t\tDIR_LstAccessDate = %x", fat_file_pool[fd].dir_entry.DIR_LstAccessDate);
    printf("\n\t\t\tDIR_FstClusHI = %x", fat_file_pool[fd].dir_entry.DIR_FstClusHI);
    printf("\n\t\t\tDIR_WrtTime = %x", fat_file_pool[fd].dir_entry.DIR_WrtTime);
    printf("\n\t\t\tDIR_WrtDate = %x", fat_file_pool[fd].dir_entry.DIR_WrtDate);
    printf("\n\t\t\tDIR_FstClusLO = %x", fat_file_pool[fd].dir_entry.DIR_FstClusLO);
    printf("\n\t\t\tDIR_FileSize = %lu Bytes", fat_file_pool[fd].dir_entry.DIR_FileSize);
}

void print_dir_entry( struct dir_entry *dir_entry ) {
    printf("\nDirectory Entry");
    printf("\n\tDIR_Name = %c%c%c%c%c%c%c%c%c%c%c", dir_entry->DIR_Name[0], dir_entry->DIR_Name[1],dir_entry->DIR_Name[2],dir_entry->DIR_Name[3],dir_entry->DIR_Name[4],dir_entry->DIR_Name[5],dir_entry->DIR_Name[6],dir_entry->DIR_Name[7],dir_entry->DIR_Name[8],dir_entry->DIR_Name[9],dir_entry->DIR_Name[10]);
    printf("\n\tDIR_Attr = %x", dir_entry->DIR_Attr);
    printf("\n\tDIR_NTRes = %x", dir_entry->DIR_NTRes);
    printf("\n\tCrtTimeTenth = %x", dir_entry->CrtTimeTenth);
    printf("\n\tDIR_CrtTime = %x", dir_entry->DIR_CrtTime);
    printf("\n\tDIR_CrtDate = %x", dir_entry->DIR_CrtDate);
    printf("\n\tDIR_LstAccessDate = %x", dir_entry->DIR_LstAccessDate);
    printf("\n\tDIR_FstClusHI = %x", dir_entry->DIR_FstClusHI);
    printf("\n\tDIR_WrtTime = %x", dir_entry->DIR_WrtTime);
    printf("\n\tDIR_WrtDate = %x", dir_entry->DIR_WrtDate);
    printf("\n\tDIR_FstClusLO = %x", dir_entry->DIR_FstClusLO);
    printf("\n\tDIR_FileSize = %lu Bytes", dir_entry->DIR_FileSize);
}

void get_fat_info( struct FAT_Info *info ) {
    memcpy( info, &(mounted.info), sizeof(struct FAT_Info) );
}

/*FAT entry functions*/
static uint32_t read_fat_entry( uint32_t cluster_num ) {
    uint32_t fat_sec_num = 0,
            ent_offset = 0;

    calc_fat_block( cluster_num, &fat_sec_num, &ent_offset );
    fat_read_block( fat_sec_num );

    if( mounted.info.type == FAT16 ) {
		PRINTF("\nfat.c: read_fat_entry( cluster_num = %lu ) = %lu", cluster_num, (uint32_t) (((uint16_t) sector_buffer[ent_offset+1]) << 8) + ((uint16_t) sector_buffer[ent_offset]));
        return (uint32_t) (((uint16_t) sector_buffer[ent_offset+1]) << 8) + ((uint16_t) sector_buffer[ent_offset]);
    } else if( mounted.info.type == FAT32 ) {
		PRINTF("\nfat.c: read_fat_entry( cluster_num = %lu ) = %lu", cluster_num, (((((uint32_t) sector_buffer[ent_offset+3]) << 24) +
                 (((uint32_t) sector_buffer[ent_offset+2]) << 16) +
                 (((uint32_t) sector_buffer[ent_offset+1]) << 8) +
                 ((uint32_t) sector_buffer[ent_offset+0]))
                 & 0x0FFFFFFF));
        /* First read a uint32_t out of the sector_buffer (first 4 lines) and then mask the highest order bit (5th line)*/
        return (((((uint32_t) sector_buffer[ent_offset+3]) << 24) +
                 (((uint32_t) sector_buffer[ent_offset+2]) << 16) +
                 (((uint32_t) sector_buffer[ent_offset+1]) << 8) +
                 ((uint32_t) sector_buffer[ent_offset+0]))
                 & 0x0FFFFFFF);
    }

	PRINTF("\nfat.c: read_fat_entry( cluster_num = %lu ) = EOC", cluster_num);
    return EOC;
}

void write_fat_entry( uint32_t cluster_num, uint32_t value ) {
    uint32_t fat_sec_num = 0,
            ent_offset = 0;

    calc_fat_block( cluster_num, &fat_sec_num, &ent_offset );
    fat_read_block( fat_sec_num );
	PRINTF("\nfat.c: write_fat_entry( cluster_num = %lu, value = %lu ) = void", cluster_num, value);

    if( mounted.info.type == FAT16 ) {
        sector_buffer[ent_offset+1] = (uint8_t) (value >> 8);
        sector_buffer[ent_offset]   = (uint8_t) (value);
    } else if( mounted.info.type == FAT32 ) {
        sector_buffer[ent_offset+3] = ((uint8_t) (value >> 24) & 0x0FFF) + (0xF000 & sector_buffer[ent_offset+3]);
        sector_buffer[ent_offset+2] = (uint8_t) (value >> 16);
        sector_buffer[ent_offset+1] = (uint8_t) (value >> 8);
        sector_buffer[ent_offset]   = (uint8_t) (value);
    }

    sector_buffer_dirty = 1;
}

static void calc_fat_block( uint32_t cur_cluster, uint32_t *fat_sec_num, uint32_t *ent_offset ) {
    uint32_t N = cur_cluster;

    if( mounted.info.type == FAT16 ) {
        *ent_offset = N * 2;
    } else if( mounted.info.type == FAT32 ) {
        *ent_offset = N * 4;
    }

    *fat_sec_num = mounted.info.BPB_RsvdSecCnt + (*ent_offset / mounted.info.BPB_BytesPerSec);
    *ent_offset = *ent_offset % mounted.info.BPB_BytesPerSec;
	PRINTF("\nfat.c: calc_fat_block( cur_cluster = %lu, *fat_sec_num = %lu, *ent_offset = %lu ) = void", cur_cluster, *fat_sec_num, *ent_offset);
}

/*Path Resolver Functions*/
static uint8_t _make_valid_name( const char *path, uint8_t start, uint8_t end, char *name ) {
    uint8_t i = 0,
            idx = 0,
            dot_found = 0;

    memset( name, 0x20, 11 );

    //printf("\n_make_valid_name() : name = ");
    for(i = 0, idx = 0; i < end-start; ++i, ++idx) {
        // Part too long
        if( idx >= 11 ) {
			PRINTF("\nfat.c: _make_valid_name( path = %s, start = %u, end = %u, name = %s ) = 2", path, start, end, name);
            return 2;
        }

        //ignore . but jump to last 3 chars of name
        if( path[start + i] == '.') {
            if( dot_found ) {
				PRINTF("\nfat.c: _make_valid_name( path = %s, start = %u, end = %u, name = %s ) = 3", path, start, end, name);
                return 3;
            }

            idx = 7;
            dot_found = 1;
            continue;
        }

        if( !dot_found && idx > 7 ) {
			PRINTF("\nfat.c: _make_valid_name( path = %s, start = %u, end = %u, name = %s ) = 4", path, start, end, name);
            return 4;
        }

        name[idx] = toupper(path[start + i]);
    }

	PRINTF("\nfat.c: _make_valid_name( path = %s, start = %u, end = %u, name = %s ) = 0", path, start, end, name);
    return 0;
}

static void pr_reset( struct PathResolver *rsolv ) {
    rsolv->start = 0;
    rsolv->end = 0;
    rsolv->path = NULL;
    memset(rsolv->name, '\0', 11);
}

static uint8_t pr_get_next_path_part( struct PathResolver *rsolv ) {
    uint16_t i = 0;

    if( rsolv->path == NULL ) {
        return 2;
    }

    rsolv->start = rsolv->end;
    rsolv->end++;
    if( rsolv->path[rsolv->start] == '/' ) {
        rsolv->start++;
    }

    for( i = rsolv->start; rsolv->path[i] != '\0'; i++ ){
        if( rsolv->path[i] != '/' ) {
            rsolv->end++;
        }

        if( rsolv->path[rsolv->end] == '/' || rsolv->path[rsolv->end] == '\0' ) {
            return _make_valid_name( rsolv->path, rsolv->start, rsolv->end, rsolv->name );
        }
    }

    return 1;
}

static uint8_t pr_is_current_path_part_a_file( struct PathResolver *rsolv ) {
    if( rsolv->path[rsolv->end] == '/') {
        return 0;
    } else if(rsolv->path[rsolv->end] == '\0' ) {
        return 1;
    }

    return 0;
}

/*Sector Buffer Functions*/
/**
 * Writes the current buffered block back to the disk if it was changed.
 */
void fat_flush() {
    if( sector_buffer_dirty ) {
        #ifdef FAT_COOPERATIVE
            if( !coop_step_allowed ) {
                next_step_type = WRITE;
                coop_switch_sp();
            } else {
                coop_step_allowed = 0;
            }
        #endif

		PRINTF("\nfat.c: fat_flush(): Flushing sector %lu", sector_buffer_addr);
        if( diskio_write_block( mounted.dev, sector_buffer_addr, sector_buffer ) != DISKIO_SUCCESS ) {
			PRINTF("\nfat.c: fat_flush(): DiskIO-Error occured");
        }

        sector_buffer_dirty = 0;
    }
}

static uint8_t fat_read_block( uint32_t sector_addr ) {
    if( sector_buffer_addr == sector_addr && sector_addr != 0 ) {
        return 0;
    }

    fat_flush();

    sector_buffer_addr = sector_addr;

    #ifdef FAT_COOPERATIVE
        if( !coop_step_allowed ) {
            next_step_type = READ;
            coop_switch_sp();
        } else {
            coop_step_allowed = 0;
        }
    #endif

	PRINTF("\nfat.c: fat_read_block( sector_addr = %lu ) = ?", sector_addr);
    return diskio_read_block( mounted.dev, sector_addr, sector_buffer );
}

static uint8_t fat_next_block() {
    fat_flush();

    /* Are we on a Cluster edge? */
    if( (sector_buffer_addr + 1) % mounted.info.BPB_SecPerClus == 0 ) {
        /* We need to change the cluster, for this we have to read the FAT entry corresponding to the current sector number */
        uint32_t entry = read_fat_entry( SECTOR_TO_CLUSTER(sector_buffer_addr) );
        /* If the returned entry is an End Of Clusterchain, return error code 128 */
        if( is_EOC( entry ) ) {
            return 128;
        }

        /* The entry is valid and we calculate the first sector number of this new cluster and read it */
        return fat_read_block( CLUSTER_TO_SECTOR(entry) );
    } else {
        /* We are still inside a cluster, so we only need to read the next sector */
        return fat_read_block( sector_buffer_addr + 1 );
    }
}

/*Mount related Functions*/
/**
 * Determines the type of the fat by using the BPB.
 */
static uint8_t determine_fat_type( struct FAT_Info *info ) {
    uint16_t RootDirSectors = ((info->BPB_RootEntCnt * 32) + (info->BPB_BytesPerSec - 1)) / info->BPB_BytesPerSec;
    uint32_t DataSec = info->BPB_TotSec - (info->BPB_RsvdSecCnt + (info->BPB_NumFATs * info->BPB_FATSz) + RootDirSectors);
    uint32_t CountofClusters = DataSec / info->BPB_SecPerClus;

    if(CountofClusters < 4085) {
        /* Volume is FAT12 */
        return FAT12;
    } else if(CountofClusters < 65525) {
        /* Volume is FAT16 */
        return FAT16;
    } else {
        /* Volume is FAT32 */
        return FAT32;
    }
}

/**
 * Parses the Bootsector of a FAT-Filesystem and validates it.
 *
 * \param buffer One sector of the filesystem, must be at least 512 Bytes long.
 * \param info The FAT_Info struct which gets populated with the FAT information.
 * \return <ul>
 *          <li> 0 : Bootsector seems okay.
 *          <li> 1 : BPB_BytesPerSec is not a power of 2
 *          <li> 2 : BPB_SecPerClus is not a power of 2
 *          <li> 4 : Bytes per Cluster is more than 32K
 *          <li> 8 : More than 2 FATs (not supported)
 *          <li> 16: BPB_TotSec is 0
 *          <li> 32: BPB_FATSz is 0
 *          <li> 64: FAT Signature isn't correct
 *         </ul>
 *         More than one error flag may be set but return is 0 on no error.
 */
static uint8_t parse_bootsector( uint8_t *buffer, struct FAT_Info *info ) {
    int ret = 0;

    info->BPB_BytesPerSec = (((uint16_t) buffer[12]) << 8) + buffer[11];
    info->BPB_SecPerClus = buffer[13];
    info->BPB_RsvdSecCnt = buffer[14] + (((uint16_t) buffer[15]) << 8);
    info->BPB_NumFATs = buffer[16];
    info->BPB_RootEntCnt = buffer[17] + (((uint16_t) buffer[18]) << 8);
    info->BPB_TotSec = buffer[19] + (((uint16_t) buffer[20]) << 8);
    if( info->BPB_TotSec == 0 ) {
        info->BPB_TotSec = buffer[32] +
            (((uint32_t) buffer[33]) << 8) +
            (((uint32_t) buffer[34]) << 16) +
            (((uint32_t) buffer[35]) << 24);
    }

    info->BPB_Media = buffer[21];
    info->BPB_FATSz =  buffer[22] + (((uint16_t) buffer[23]) << 8);
    if( info->BPB_FATSz == 0 ) {
        info->BPB_FATSz = buffer[36] +
            (((uint32_t) buffer[37]) << 8) +
            (((uint32_t) buffer[38]) << 16) +
            (((uint32_t) buffer[39]) << 24);
    }

    info->BPB_RootClus =  buffer[44] +
        (((uint32_t) buffer[45]) << 8) +
        (((uint32_t) buffer[46]) << 16) +
        (((uint32_t) buffer[47]) << 24);

    if( is_a_power_of_2( info->BPB_BytesPerSec ) != 0) {
        ret += 1;
    }

    if( is_a_power_of_2( info->BPB_SecPerClus ) != 0) {
        ret += 2;
    }

    if( info->BPB_BytesPerSec * info->BPB_SecPerClus > 32 * ((uint32_t) 1024) ) {
        ret += 4;
    }

    if( info->BPB_NumFATs > 2 ) {
        ret += 8;
    }

    if( info->BPB_TotSec == 0 ) {
        ret += 16;
    }

    if( info->BPB_FATSz == 0 ) {
        ret += 32;
    }

    if( buffer[510] != 0x55 || buffer[511] != 0xaa ) {
        ret += 64;
    }

    return ret;
}

uint8_t fat_mount_device( struct diskio_device_info *dev ) {
    uint32_t RootDirSectors = 0;

    if (mounted.dev != 0) {
        fat_umount_device();
    }

    //read first sector into buffer
    diskio_read_block( dev, 0, sector_buffer );

    //parse bootsector
    if( parse_bootsector( sector_buffer, &(mounted.info) ) != 0 ) {
        return 1;
    }

    //call determine_fat_type
    mounted.info.type = determine_fat_type( &(mounted.info) );

    //return 2 if unsupported
    if( mounted.info.type != FAT16 && mounted.info.type != FAT32 ) {
        return 2;
    }

    mounted.dev = dev;

    //sync every FAT to the first on mount
    //Addendum: Takes so frigging long to do that
    //fat_sync_fats();

    //Calculated the first_data_sector
    RootDirSectors = ((mounted.info.BPB_RootEntCnt * 32) + (mounted.info.BPB_BytesPerSec - 1)) / mounted.info.BPB_BytesPerSec;
    mounted.first_data_sector = mounted.info.BPB_RsvdSecCnt + (mounted.info.BPB_NumFATs * mounted.info.BPB_FATSz) + RootDirSectors;

    return 0;
}

void fat_umount_device() {
    uint8_t i = 0;

    // Write last buffer
    fat_flush();

    // Write second FAT
    fat_sync_fats();

    // invalidate file-descriptors
    for(i = 0; i < FAT_FD_POOL_SIZE; i++) {
        fat_fd_pool[i].file = 0;
    }

    // Reset the device pointer
    mounted.dev = 0;
}

/*CFS frontend functions*/
int cfs_open(const char *name, int flags) {
    // get FileDescriptor
    int fd = -1;
    uint8_t i = 0;
    struct dir_entry dir_ent;
	PRINTF("\nfat.c: cfs_open( name = %s, flags = %x) = ?", name, flags);

#ifndef FAT_COOPERATIVE
    for( i = 0; i < FAT_FD_POOL_SIZE; i++ ) {
        if( fat_fd_pool[i].file == 0 ) {
            fd = i;
            break;
        }
    }

    /*No free FileDesciptors available*/
    if( fd == -1 ) {
		PRINTF("\nfat.c: cfs_open(): No free FileDescriptors available!");
        return fd;
    }
#else
        fd = queue[queue_start].ret_value;
#endif

    // find file on Disk
    if( !get_dir_entry( name, &dir_ent, &fat_file_pool[fd].dir_entry_sector, &fat_file_pool[fd].dir_entry_offset,(flags & CFS_WRITE) || (flags & CFS_APPEND) ) ) {
		PRINTF("\nfat.c: cfs_open(): Could not fetch the directory entry!");
        return -1;
    }

    if( !_is_file( &dir_ent ) ) {
		PRINTF("\nfat.c: cfs_open(): Directory entry is not a file!");
        return -1;
    }

    if( !_cfs_flags_ok( flags, &dir_ent ) ) {
		PRINTF("\nfat.c: cfs_open(): Invalid flags!!");
        return -1;
    }

    fat_file_pool[fd].cluster = dir_ent.DIR_FstClusLO + (((uint32_t) dir_ent.DIR_FstClusHI) << 16);
    fat_file_pool[fd].nth_cluster = fat_file_pool[fd].cluster;
    fat_file_pool[fd].n = 0;
    fat_fd_pool[fd].file = &(fat_file_pool[fd]);
    fat_fd_pool[fd].flags = (uint8_t) flags;

    // put read/write position in the right spot
    fat_fd_pool[fd].offset = 0;
    memcpy( &(fat_file_pool[fd].dir_entry), &dir_ent, sizeof(struct dir_entry) );

    if( flags & CFS_APPEND ) {
		PRINTF("\nfat.c: cfs_open(): Seek to end of file (APPEND)!");
        cfs_seek( fd, CFS_SEEK_END, 0 );
    }

    // return FileDescriptor
	PRINTF("\nfat.c: cfs_open( name = %s, flags = %x) = %d", name, flags, fd);
    return fd;
}

void cfs_close(int fd) {
    if( fd < 0 || fd >= FAT_FD_POOL_SIZE ) {
        return;
    }

    if( fat_fd_pool[fd].file == NULL ) {
        return;
    }

    update_dir_entry( fd );
    fat_flush( fd );
    fat_fd_pool[fd].file = NULL;
}

int cfs_read(int fd, void *buf, unsigned int len) {
    uint32_t offset = fat_fd_pool[fd].offset % mounted.info.BPB_BytesPerSec;
    uint32_t clusters = (fat_fd_pool[fd].offset / mounted.info.BPB_BytesPerSec) / mounted.info.BPB_SecPerClus;
    uint8_t clus_offset = (fat_fd_pool[fd].offset / mounted.info.BPB_BytesPerSec) % mounted.info.BPB_SecPerClus;
    uint16_t i, j = 0;
    uint8_t *buffer = (uint8_t *) buf;

    if( fd < 0 || fd >= FAT_FD_POOL_SIZE ) {
        return -1;
    }

    /*Special case of empty file, cluster is zero, no data*/
    if( fat_file_pool[fd].cluster == 0 ) {
        return 0;
    }

    if( !(fat_fd_pool[fd].flags & CFS_READ) ) {
        return -1;
    }

    while( load_next_sector_of_file( fd, clusters, clus_offset, 0 ) == 0 ) {
        for( i = offset; i < 512 && j < len; i++,j++,fat_fd_pool[fd].offset++ ) {
            buffer[j] = sector_buffer[i];
        }

        if( (clus_offset + 1) % mounted.info.BPB_SecPerClus == 0 ) {
            clus_offset = 0;
            clusters++;
        } else {
            clus_offset++;
        }

        if( j >= len ) {
            break;
        }
    }
    return (int) j;
}

int cfs_write(int fd, const void *buf, unsigned int len) {
    uint16_t offset = fat_fd_pool[fd].offset % (uint32_t)mounted.info.BPB_BytesPerSec;
    uint32_t clusters = (fat_fd_pool[fd].offset / mounted.info.BPB_BytesPerSec) / mounted.info.BPB_SecPerClus;
    uint8_t clus_offset = (fat_fd_pool[fd].offset / mounted.info.BPB_BytesPerSec) % mounted.info.BPB_SecPerClus;
    uint16_t i, j = 0;
    uint8_t *buffer = (uint8_t *) buf;

    while( load_next_sector_of_file( fd, clusters, clus_offset, 1 ) == 0 ) {
		PRINTF("\nfat.c: cfs_write(): Writing in sector %lu", sector_buffer_addr);
        for( i = offset; i < mounted.info.BPB_BytesPerSec && j < len; i++,j++,fat_fd_pool[fd].offset++ ) {
            #ifndef FAT_COOPERATIVE
                sector_buffer[i] = buffer[j];
            #else
                sector_buffer[i] = get_item_from_buffer(buffer, j);
            #endif
            if( fat_fd_pool[fd].offset == fat_file_pool[fd].dir_entry.DIR_FileSize ) {
                fat_file_pool[fd].dir_entry.DIR_FileSize++;
            } else if( fat_fd_pool[fd].offset > fat_file_pool[fd].dir_entry.DIR_FileSize ) {
				fat_file_pool[fd].dir_entry.DIR_FileSize = fat_fd_pool[fd].offset;
			}
        }

        sector_buffer_dirty = 1;
        offset = 0;
        clus_offset = (clus_offset + 1) % mounted.info.BPB_SecPerClus;
        if(  clus_offset == 0 ) {
            clusters++;
        }

        if( j >= len ) {
            break;
        }
    }

    return j;
}

cfs_offset_t cfs_seek(int fd, cfs_offset_t offset, int whence) {
    if( fd < 0 || fd >= FAT_FD_POOL_SIZE ) {
        return -1;
    }

    switch(whence) {
        case CFS_SEEK_SET:
            fat_fd_pool[fd].offset = offset;
            break;
        case CFS_SEEK_CUR:
            fat_fd_pool[fd].offset += offset;
            break;
        case CFS_SEEK_END:
            fat_fd_pool[fd].offset = (fat_file_pool[fd].dir_entry.DIR_FileSize - 1) + offset;
            break;
        default:
            break;
    }

    if( fat_fd_pool[fd].offset >= fat_file_pool[fd].dir_entry.DIR_FileSize ) {
        fat_fd_pool[fd].offset = (fat_file_pool[fd].dir_entry.DIR_FileSize - 1);
    }

    if( fat_fd_pool[fd].offset < 0 ) {
        fat_fd_pool[fd].offset = 0;
    }

    return fat_fd_pool[fd].offset;
}

int cfs_remove(const char *name) {
    struct dir_entry dir_ent;
    uint32_t sector;
    uint16_t offset;

    if( !get_dir_entry( name, &dir_ent, &sector, &offset, 0 ) ) {
        return -1;
    }

    if( _is_file( &dir_ent ) ) {
        reset_cluster_chain( &dir_ent );
        remove_dir_entry( sector, offset );
        fat_flush();
        return 0;
    }

    return -1;
}

int cfs_opendir(struct cfs_dir *dirp, const char *name) {
    struct dir_entry dir_ent;
    uint32_t sector;
    uint16_t offset;
    uint32_t dir_cluster = get_dir_entry( name, &dir_ent, &sector, &offset, 0 );

    cfs_readdir_offset = 0;

    if( dir_cluster == 0 ) {
        return -1;
    }

    memcpy( dirp, &dir_ent, sizeof(struct dir_entry) );
    return 0;
}

int cfs_readdir(struct cfs_dir *dirp, struct cfs_dirent *dirent) {
    struct dir_entry *dir_ent = (struct dir_entry *) dirp;
    struct dir_entry entry;

    { /* Get the next directory_entry */
        uint32_t dir_off = cfs_readdir_offset * 32;
        uint16_t cluster_num = dir_off / mounted.info.BPB_SecPerClus;
        uint32_t cluster;

        cluster = find_nth_cluster( (((uint32_t) dir_ent->DIR_FstClusHI) << 16) + dir_ent->DIR_FstClusLO, (uint32_t) cluster_num );
        if( cluster == 0 ) {
            return -1;
        }

        if( fat_read_block( CLUSTER_TO_SECTOR(cluster) + dir_off / mounted.info.BPB_BytesPerSec ) != 0 ) {
            return -1;
        }

        memcpy( &entry, &(sector_buffer[dir_off % mounted.info.BPB_BytesPerSec]), sizeof(struct dir_entry) );
    }

    make_readable_entry( &entry, dirent );
    dirent->size = entry.DIR_FileSize;
    cfs_readdir_offset++;
    return 0;
}

void cfs_closedir(struct cfs_dir *dirp) {
    cfs_readdir_offset = 0;
}

/*Dir_entry Functions*/
static uint8_t lookup( const char *name, struct dir_entry *dir_entry, uint32_t *dir_entry_sector, uint16_t *dir_entry_offset ) {
	uint16_t i = 0;
			PRINTF("\nfat.c: BEGIN lookup( name = %c%c%c%c%c%c%c%c%c%c%c, dir_entry = %p, *dir_entry_sector = %lu, *dir_entry_offset = %u) = ?", name[0], name[1], name[2], name[3], name[4], name[5], name[6], name[7], name[8], name[9], name[10], dir_entry, *dir_entry_sector, *dir_entry_offset);
	for(;;) {
		for( i = 0; i < 512; i+=32 ) {
			PRINTF("\nfat.c: lookup(): name = %c%c%c%c%c%c%c%c%c%c%c", name[0], name[1], name[2], name[3], name[4], name[5], name[6], name[7], name[8], name[9], name[10]);
			PRINTF("\nfat.c: lookup(): sec_buf = %c%c%c%c%c%c%c%c%c%c%c", sector_buffer[i+0], sector_buffer[i+1], sector_buffer[i+2], sector_buffer[i+3], sector_buffer[i+4], sector_buffer[i+5], sector_buffer[i+6], sector_buffer[i+7], sector_buffer[i+8], sector_buffer[i+9], sector_buffer[i+10]);
			if( memcmp( name, &(sector_buffer[i]), 11 ) == 0 ) {
				memcpy( dir_entry, &(sector_buffer[i]), sizeof(struct dir_entry) );
				*dir_entry_sector = sector_buffer_addr;
				*dir_entry_offset = i;
			PRINTF("\nfat.c: END lookup( name = %c%c%c%c%c%c%c%c%c%c%c, dir_entry = %p, *dir_entry_sector = %lu, *dir_entry_offset = %u) = 0", name[0], name[1], name[2], name[3], name[4], name[5], name[6], name[7], name[8], name[9], name[10], dir_entry, *dir_entry_sector, *dir_entry_offset);
				return 0;
			}

			// There are no more entries in this directory
			if( sector_buffer[i] == 0x00 ) {
				PRINTF("\nfat.c: lookup(): No more directory entries");
			PRINTF("\nfat.c: END lookup( name = %c%c%c%c%c%c%c%c%c%c%c, dir_entry = %p, *dir_entry_sector = %lu, *dir_entry_offset = %u) = 1", name[0], name[1], name[2], name[3], name[4], name[5], name[6], name[7], name[8], name[9], name[10], dir_entry, *dir_entry_sector, *dir_entry_offset);
				return 1;
			}
		}

		if( fat_next_block() != 0 ) {
			PRINTF("\nfat.c: END lookup( name = %c%c%c%c%c%c%c%c%c%c%c, dir_entry = %p, *dir_entry_sector = %lu, *dir_entry_offset = %u) = 2", name[0], name[1], name[2], name[3], name[4], name[5], name[6], name[7], name[8], name[9], name[10], dir_entry, *dir_entry_sector, *dir_entry_offset);
			return 2;
		}
	}

	PRINTF("\nfat.c: END lookup( name = %c%c%c%c%c%c%c%c%c%c%c, dir_entry = %p, *dir_entry_sector = %lu, *dir_entry_offset = %u) = 0", name[0], name[1], name[2], name[3], name[4], name[5], name[6], name[7], name[8], name[9], name[10], dir_entry, *dir_entry_sector, *dir_entry_offset);
	return 0;
}

static uint8_t get_dir_entry( const char *path, struct dir_entry *dir_ent, uint32_t *dir_entry_sector, uint16_t *dir_entry_offset, uint8_t create ) {
	uint32_t first_root_dir_sec_num = 0;
	uint32_t file_sector_num = 0;
	uint8_t i = 0;
	struct PathResolver pr;
	PRINTF("\nfat.c: get_dir_entry( path = %s, dir_ent = %p, *dir_entry_sector = %lu, *dir_entry_offset = %u, create = %u ) = ?", path, dir_ent, *dir_entry_sector, *dir_entry_offset, create);

	pr_reset( &pr );
	pr.path = path;

	if( mounted.info.type == FAT16 ) {
		// calculate the first cluster of the root dir
		first_root_dir_sec_num = mounted.info.BPB_RsvdSecCnt + (mounted.info.BPB_NumFATs * mounted.info.BPB_FATSz); // TODO Verify this is correct
	} else if( mounted.info.type == FAT32 ) {
		// BPB_RootClus is the first cluster of the root dir
        first_root_dir_sec_num = CLUSTER_TO_SECTOR( mounted.info.BPB_RootClus );
	}
	PRINTF("\nfat.c: get_dir_entry(): first_root_dir_sec_num = %lu", first_root_dir_sec_num);

	file_sector_num = first_root_dir_sec_num;
	for(i = 0; pr_get_next_path_part( &pr ) == 0 && i < 255; i++) {
		fat_read_block( file_sector_num );
		if( lookup( pr.name, dir_ent, dir_entry_sector, dir_entry_offset ) != 0 ) {
			PRINTF("\nfat.c: get_dir_entry(): Current path part doesn't exist!");
            if( pr_is_current_path_part_a_file( &pr ) && create ) {
				PRINTF("\nfat.c: get_dir_entry(): Current path part describes a file and it should be created!");
                memset( dir_ent, 0, sizeof(struct dir_entry) );
                memcpy( dir_ent->DIR_Name, pr.name, 11 );
                dir_ent->DIR_Attr = 0;
                return add_directory_entry_to_current( dir_ent, dir_entry_sector, dir_entry_offset );
            }
			return 0;
		}
        file_sector_num = CLUSTER_TO_SECTOR( dir_ent->DIR_FstClusLO + (((uint32_t) dir_ent->DIR_FstClusHI) << 16) );
		PRINTF("\nfat.c: get_dir_entry(): file_sector_num = %lu", file_sector_num);
	}

	if( file_sector_num == first_root_dir_sec_num ) {
		return 0;
	}

	return 1;
}

static uint8_t add_directory_entry_to_current( struct dir_entry *dir_ent, uint32_t *dir_entry_sector, uint16_t *dir_entry_offset ) {
	uint16_t i = 0;
	uint8_t ret = 0;

	PRINTF("\nfat.c: add_directory_entry_to_current( dir_ent = %p, *dir_entry_sector = %lu, *dir_entry_offset = %u ) = ?", dir_ent, *dir_entry_sector, *dir_entry_offset);
	for(;;) {
		for( i = 0; i < 512; i+=32 ) {
			if( sector_buffer[i] == 0x00 || sector_buffer[i] == 0xE5 ) {
				memcpy( &(sector_buffer[i]), dir_ent, sizeof(struct dir_entry) );
				sector_buffer_dirty = 1;
				*dir_entry_sector = sector_buffer_addr;
				*dir_entry_offset = i;
				PRINTF("\nfat.c: add_directory_entry_to_current(): Found empty directory entry! *dir_entry_sector = %lu, *dir_entry_offset = %u", *dir_entry_sector, *dir_entry_offset);
				return 1;
			}
		}

		PRINTF("\nfat.c: add_directory_entry_to_current(): No free entry in current sector (sector_buffer_addr = %lu) reading next sector!", sector_buffer_addr);
		if( (ret = fat_next_block()) != 0 ) {
			if( ret == 128 ) {
                uint32_t free_cluster = get_free_cluster( SECTOR_TO_CLUSTER( sector_buffer_addr ) );
				PRINTF("\nfat.c: add_directory_entry_to_current(): The directory cluster chain is too short, we need to add another cluster!");

                write_fat_entry( SECTOR_TO_CLUSTER( sector_buffer_addr ), free_cluster );
                write_fat_entry( free_cluster, EOC );
				PRINTF("\nfat.c: add_directory_entry_to_current(): cluster %lu added to chain of sector_buffer_addr cluster %lu", free_cluster, SECTOR_TO_CLUSTER( sector_buffer_addr ));

                if( fat_read_block( CLUSTER_TO_SECTOR( free_cluster ) ) == 0 ) {
					memcpy( &(sector_buffer[0]), dir_ent, sizeof(struct dir_entry) );
                    sector_buffer_dirty = 1;
					*dir_entry_sector = sector_buffer_addr;
					*dir_entry_offset = 0;
					PRINTF("\nfat.c: add_directory_entry_to_current(): read of the newly added cluster successful! *dir_entry_sector = %lu, *dir_entry_offset = %u", *dir_entry_sector, *dir_entry_offset);
					return 1;
				}
			}
			return 0;
		}
	}

	return 0;
}

static void update_dir_entry( int fd ) {
	PRINTF("\nfat.c: update_dir_entry( fd = %d ) = void ", fd);
	if( fat_read_block( fat_file_pool[fd].dir_entry_sector ) != 0 ) {
		PRINTF("\nfat.c: update_dir_entry(): error reading the sector containing the directory entry");
		return;
	}

	memcpy( &(sector_buffer[fat_file_pool[fd].dir_entry_offset]), &(fat_file_pool[fd].dir_entry), sizeof(struct dir_entry) );
	sector_buffer_dirty = 1;
}

static void remove_dir_entry( uint32_t dir_entry_sector, uint16_t dir_entry_offset ) {
	PRINTF("\nfat.c: remove_dir_entry( dir_entry_sector = %lu, dir_entry_offset = %u ) = void ", dir_entry_sector, dir_entry_offset);
	if( fat_read_block( dir_entry_sector ) != 0 ) {
		PRINTF("\nfat.c: remove_dir_entry(): error reading the sector containing the directory entry");
		return;
	}

	memset( &(sector_buffer[dir_entry_offset]), 0, sizeof(struct dir_entry) );
	sector_buffer[dir_entry_offset] = 0xE5;
	sector_buffer_dirty = 1;
}

/*FAT Implementation Functions*/
static uint8_t load_next_sector_of_file( int fd, uint32_t clusters, uint8_t clus_offset, uint8_t write ) {
    uint32_t cluster = 0;
	PRINTF("\nfat.c: load_next_sector_of_file( fd = %d, clusters = %lu, clus_offset = %u, write = %u ) = ?", fd, clusters, clus_offset, write);

    //If we know the nth Cluster already we do not have to recalculate it
    if( clusters == fat_file_pool[fd].n ) {
		PRINTF("\nfat.c: load_next_sector_of_file(): we know nth cluster already");
        cluster = fat_file_pool[fd].nth_cluster;
    //If we are now at the nth-1 Cluster it is easy to get the next cluster
    } else if( clusters == fat_file_pool[fd].n + 1) {
		PRINTF("\nfat.c: load_next_sector_of_file(): we need the cluster n and are at n-1");
        cluster = read_fat_entry( fat_file_pool[fd].nth_cluster );
    //Somehow our cluster-information is out of sync. We have to calculate the cluster the hard way.
    } else {
		PRINTF("\nfat.c: load_next_sector_of_file(): We are somewhere else, need to iterate the chain until nth cluster");
        cluster = find_nth_cluster( fat_file_pool[fd].cluster, clusters );
    }
	PRINTF("\nfat.c: load_next_sector_of_file(): fat_file_pool[%d].nth_cluster = %lu, fat_file_pool[%d].n = %lu", fd, fat_file_pool[fd].nth_cluster, fd, fat_file_pool[fd].n);

    // If there is no cluster allocated to the file or the current cluster is EOC then add another cluster to the file
    if( cluster == 0 || is_EOC( cluster ) ) {
		PRINTF("\nfat.c: load_next_sector_of_file(): Either file is empty or current cluster is EOC!");
        if( write ) {
			PRINTF("\nfat.c: load_next_sector_of_file(): write flag enabled! adding cluster to file!");
            add_cluster_to_file( fd );
            // Remember that after the add_cluster_to_file-Function the nth_cluster and n is set to the added cluster
            cluster = fat_file_pool[fd].nth_cluster;
        } else {
            return 1;
        }
    } else {
        fat_file_pool[fd].nth_cluster = cluster;
        fat_file_pool[fd].n = clusters;
	}

    return fat_read_block( CLUSTER_TO_SECTOR(cluster) + clus_offset );
}

/*FAT Interface Functions*/
uint32_t fat_file_size(int fd) {
    if( fd < 0 || fd >= FAT_FD_POOL_SIZE ) {
        return 0;
    }

    if( fat_fd_pool[fd].file == NULL ) {
        return 0;
    }

    return fat_file_pool[fd].dir_entry.DIR_FileSize;
}

/**
 * Syncs every FAT with the first.
 */
void fat_sync_fats() {
    uint8_t fat_number;
    uint32_t fat_block;

    fat_flush();

    for(fat_block = 0; fat_block < mounted.info.BPB_FATSz; fat_block++) {
        diskio_read_block( mounted.dev, fat_block + mounted.info.BPB_RsvdSecCnt, sector_buffer );
        for(fat_number = 2; fat_number <= mounted.info.BPB_NumFATs; fat_number++) {
            diskio_write_block( mounted.dev, (fat_block + mounted.info.BPB_RsvdSecCnt) + ((fat_number - 1) * mounted.info.BPB_FATSz), sector_buffer );
        }
    }
}

/*Helper Functions*/
static void make_readable_entry( struct dir_entry *dir, struct cfs_dirent *dirent ) {
    uint8_t i, j;

    for( i = 0, j = 0; i < 11; i++ ) {
        if( dir->DIR_Name[i] != ' ' ) {
            dirent->name[j] = dir->DIR_Name[i];
            j++;
        }

        if( i == 7 ) {
            dirent->name[j] = '.';
            j++;
        }
    }
}

/**
 * Tests if the given value is a power of 2.
 *
 * \param value Number which should be testet if it is a power of 2.
 * \return 1 on failure and 0 if value is a power of 2.
 */
uint8_t is_a_power_of_2( uint32_t value ) {
    uint32_t test = 1;
    uint8_t i = 0;

    if( value == 0 ) {
        return 0;
    }

    for( i = 0; i < 32; ++i ) {
        if( test == value ) {
            return 0;
        }

        test = test << 1;
    }
    return 1;
}

/**
 * Rounds the value down to the next lower power of 2.
 *
 * \param value The number which should be rounded down.
 * \return the next lower number which is a power of 2
 */
uint32_t round_down_to_power_of_2( uint32_t value ) {
    uint32_t po2 = ((uint32_t) 1) << 31;

    while( value < po2 ) {
        po2 = po2 >> 1;
    }

    return po2;
}

static uint8_t _is_file( struct dir_entry *dir_ent ) {
    if( dir_ent->DIR_Attr & ATTR_DIRECTORY ) {
        return 0;
    }

    if( dir_ent->DIR_Attr & ATTR_VOLUME_ID ) {
        return 0;
    }

    return 1;
}

static uint8_t _cfs_flags_ok( int flags, struct dir_entry *dir_ent ) {
    if( ((flags & CFS_APPEND) || (flags & CFS_WRITE)) && (dir_ent->DIR_Attr & ATTR_READ_ONLY) ) {
        return 0;
    }

    return 1;
}
