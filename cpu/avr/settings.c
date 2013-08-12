/*
 * Copyright (c) 2013, TU Braunschweig
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
 */

#include <stdbool.h>
//#include <sys/param.h>
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#include <avr/io.h>
#include "settings.h"
#include "dev/eeprom.h"
#include <stdio.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <avr/wdt.h>
#include "contiki.h"

#ifndef SETTINGS_TOP_ADDR
#define SETTINGS_TOP_ADDR	(E2END-4)	//!< Defaults to end of EEPROM, minus 4 bytes for avrdude erase count
#endif

#ifndef SETTINGS_MAX_SIZE
#define SETTINGS_MAX_SIZE	(1024)	//!< Defaults to 1KB
#endif

//#pragma mark -
//#pragma mark Private Functions

typedef struct {
  uint8_t size_extra;
  uint8_t size_low;
  uint8_t size_check;
  settings_key_t key;
} item_header_t;
/*----------------------------------------------------------------------------*/
inline static bool
settings_is_item_valid_(eeprom_addr_t item_addr)
{
  item_header_t header = {};

  if (item_addr == EEPROM_NULL)
    return false;

  //	if((SETTINGS_TOP_ADDR-item_addr)>=SETTINGS_MAX_SIZE-3)
  //		return false;

  eeprom_read(
          item_addr + 1 - sizeof (header),
          (unsigned char*) &header,
          sizeof (header)
          );

  if ((uint8_t) header.size_check != (uint8_t) ~header.size_low)
    return false;

  // TODO: Check length as well

  return true;
}
/*----------------------------------------------------------------------------*/
inline static settings_key_t
settings_get_key_(eeprom_addr_t item_addr)
{
  item_header_t header;

  eeprom_read(
          item_addr + 1 - sizeof (header),
          (unsigned char*) &header,
          sizeof (header)
          );

  if ((uint8_t) header.size_check != (uint8_t) ~header.size_low)
    return SETTINGS_INVALID_KEY;

  return header.key;
}
/*----------------------------------------------------------------------------*/
inline static size_t
settings_get_value_length_(eeprom_addr_t item_addr)
{
  item_header_t header;
  size_t ret = 0;

  eeprom_read(
          item_addr + 1 - sizeof (header),
          (unsigned char*) &header,
          sizeof (header)
          );

  if ((uint8_t) header.size_check != (uint8_t) ~header.size_low)
    goto bail;

  ret = header.size_low;

  if (ret & (1 << 7)) {
    ret = ((ret&~(1 << 7)) << 8) | header.size_extra;
  }

bail:
  return ret;
}
/*----------------------------------------------------------------------------*/
inline static eeprom_addr_t
settings_get_value_addr_(eeprom_addr_t item_addr)
{
  size_t len = settings_get_value_length_(item_addr);

  if (len > 128)
    return item_addr + 1 - sizeof (item_header_t) - len;

  return item_addr + 1 - sizeof (item_header_t) + 1 - len;
}
/*----------------------------------------------------------------------------*/
inline static eeprom_addr_t
settings_next_item_(eeprom_addr_t item_addr)
{
  return settings_get_value_addr_(item_addr) - 1;
}
/*----------------------------------------------------------------------------*/
//#pragma mark -
//#pragma mark Public Functions
bool
settings_check(settings_key_t key, uint8_t index)
{
  bool ret = false;
  eeprom_addr_t current_item = SETTINGS_TOP_ADDR;

  for (current_item = SETTINGS_TOP_ADDR; settings_is_item_valid_(current_item); current_item = settings_next_item_(current_item)) {
    if (settings_get_key_(current_item) == key) {
      if (!index) {
        ret = true;
        break;
      } else {
        // Nope, keep looking
        index--;
      }
    }
  }

  return ret;
}
/*----------------------------------------------------------------------------*/
settings_status_t
settings_get(settings_key_t key, uint8_t index, unsigned char* value, size_t* value_size)
{
  settings_status_t ret = SETTINGS_STATUS_NOT_FOUND;
  eeprom_addr_t current_item = SETTINGS_TOP_ADDR;

  for (current_item = SETTINGS_TOP_ADDR; settings_is_item_valid_(current_item); current_item = settings_next_item_(current_item)) {
    if (settings_get_key_(current_item) == key) {
      if (index == 0) {
        // We found it!

        if (*value_size == 0) {
          *value_size = settings_get_value_length_(current_item);
        } else {
          *value_size = MIN(*value_size, settings_get_value_length_(current_item));
        }
        eeprom_read(
                settings_get_value_addr_(current_item),
                value,
                *value_size
                );
        ret = SETTINGS_STATUS_OK;
        break;
      } else {
        // Nope, keep looking
        index--;
      }
    }
  }

  return ret;
}
/*----------------------------------------------------------------------------*/
settings_status_t
settings_add(settings_key_t key, const unsigned char* value, size_t value_size)
{
  settings_status_t ret = SETTINGS_STATUS_FAILURE;
  eeprom_addr_t current_item = SETTINGS_TOP_ADDR;
  item_header_t header;

  // Find end of list
  for (current_item = SETTINGS_TOP_ADDR; settings_is_item_valid_(current_item); current_item = settings_next_item_(current_item));

  if (current_item == EEPROM_NULL)
    goto bail;

  // TODO: size check!

  header.key = key;

  if (value_size < 0x80) {
    // If the value size is less than 128, then
    // we can get away with only using one byte
    // as the size.
    header.size_low = value_size;
  } else if (value_size <= SETTINGS_MAX_VALUE_SIZE) {
    // If the value size of larger than 128,
    // then we need to use two bytes. Store
    // the most significant 7 bits in the first
    // size byte (with MSB set) and store the
    // least significant bits in the second
    // byte (with LSB clear)
    header.size_low = (value_size >> 7) | 0x80;
    header.size_extra = value_size & ~0x80;
  } else {
    // Value size way too big!
    goto bail;
  }

  header.size_check = ~header.size_low;

  // Write the header first
  eeprom_write(
          current_item + 1 - sizeof (header),
          (unsigned char*) &header,
          sizeof (header)
          );

  // Sanity check, remove once confident
  if (settings_get_value_length_(current_item) != value_size) {
    goto bail;
  }

  // Now write the data
  eeprom_write(
          settings_get_value_addr_(current_item),
          (unsigned char*) value,
          value_size
          );

  ret = SETTINGS_STATUS_OK;

bail:
  return ret;
}
/*----------------------------------------------------------------------------*/
settings_status_t
settings_set(settings_key_t key, const unsigned char* value, size_t value_size)
{
  settings_status_t ret = SETTINGS_STATUS_FAILURE;
  eeprom_addr_t current_item = SETTINGS_TOP_ADDR;

  /* Iterates over settings items. */
  for (current_item = SETTINGS_TOP_ADDR; settings_is_item_valid_(current_item); current_item = settings_next_item_(current_item)) {
    if (settings_get_key_(current_item) == key) {
      break;
    }
  }

  /* Create new item if not existig or invalid */
  if ((current_item == EEPROM_NULL) || !settings_is_item_valid_(current_item)) {
    ret = settings_add(key, value, value_size);
    goto bail;
  }

  if (value_size != settings_get_value_length_(current_item)) {
    // Requires the settings store to be shifted. Currently unimplemented.
    goto bail;
  }

  // Now write the data
  eeprom_write(
          settings_get_value_addr_(current_item),
          (unsigned char*) value,
          value_size
          );

  ret = SETTINGS_STATUS_OK;

bail:
  return ret;
}
/*----------------------------------------------------------------------------*/
settings_status_t
settings_delete(settings_key_t key, uint8_t index)
{

  //settings_status_t ret = SETTINGS_STATUS_NOT_FOUND;  ///@todo: returns INVALID only?
  eeprom_addr_t current_item = SETTINGS_TOP_ADDR;
  eeprom_addr_t tmp_addr;
  uint8_t tmp_data;
  uint16_t tmp_offset;
  uint8_t shift_amount = 0;

  for (current_item = SETTINGS_TOP_ADDR; settings_is_item_valid_(current_item); current_item = settings_next_item_(current_item)) {
    if (settings_get_key_(current_item) == key) {
      if (index) // skip as long as we did not reach the index
      {
        index--;
        continue;
      }

      shift_amount = (current_item - settings_get_value_addr_(current_item) - 1) + settings_get_value_length_(current_item);

      // printf("base address: %X, value address: %x, header length: %d\n", current_item, settings_get_value_addr_(current_item), );

      //printf("index found, shift_count: %d, address: %X\n", shift_count, current_item - shift_count);

      break;
    }
  }

  if (shift_amount > 0) {
    printf("shift_amount: %d\n", shift_amount);

    do {
      current_item = settings_next_item_(current_item);

      if (!settings_is_item_valid_(current_item)) {
        break;
      }

      tmp_addr = current_item;
      printf("found a valid item to shift at address %X\n", current_item);

      for (tmp_offset = 0; tmp_offset < (current_item - settings_get_value_addr_(current_item)) + settings_get_value_length_(current_item); tmp_offset++) {
        eeprom_read(tmp_addr - tmp_offset, (unsigned char *) &tmp_data, 1);
        printf("data[%d]: %X", tmp_offset, tmp_data);
        //eeprom_read(tmp_addr - tmp_offset + shift_amount, (const unsigned char*) &tmp_data, 1);
        // printf("-> %X\n", tmp_data);
        eeprom_write_byte((uint8_t*) (tmp_addr - tmp_offset + shift_amount), tmp_data);
      }
    } while (1);

    for (tmp_offset = 1; tmp_offset <= shift_amount; tmp_offset++) {
      eeprom_write_byte((uint8_t*) (current_item + tmp_offset), 0xFF);
    }
  }

  return SETTINGS_STATUS_UNIMPLEMENTED;
}
/*----------------------------------------------------------------------------*/
void
settings_wipe(void)
{
  size_t i = SETTINGS_TOP_ADDR - SETTINGS_MAX_SIZE;
  for (; i <= SETTINGS_TOP_ADDR; i++) {
    eeprom_write_byte((uint8_t*) i, 0xFF);
    wdt_reset();
  }
}
/*----------------------------------------------------------------------------*/
