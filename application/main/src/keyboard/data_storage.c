/*
Copyright (C) 2019 Jim Jiang <jim@lotlab.org>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <string.h>

#include "data_storage.h"

#include "app_scheduler.h"
#include "fds.h"
#include "nrf.h"

// keymap
#include "keymap.h"
#include "keymap_common.h"

// actionmap
#include "actionmap.h"

// macro
#include "action_macro.h"

#ifdef ACTIONMAP_ENABLE
#define SINGLE_KEY_SIZE 2
#else
#define SINGLE_KEY_SIZE 1
#endif

#define MAX_LAYER 8
#define MAX_FN_KEYS 32
#define MAX_MACRO_SIZE 256
#define FILE_ID 0x0514

#define GET_WORD(_i) ((_i - 1) / 4 + 1)

#define REGISTER_FDS_BLOCK(_name, _size_word, _record_key)  \
    __ALIGN(4)                                              \
    uint8_t _name##_block[(_size_word * 4)] = { 0 }; \
    static fds_record_desc_t _name##_record_desc = { 0 };   \
    static fds_record_t _name##_record = {                  \
        .file_id = FILE_ID,                                 \
        .key = _record_key,                                 \
        .data = {                                           \
            .p_data = &_name##_block,                       \
            .length_words = _size_word,                     \
        }                                                   \
    };

#ifdef KEYMAP_STORAGE
#pragma region KEYMAP
#define LAYER_SIZE (MATRIX_ROWS * MATRIX_COLS * SINGLE_KEY_SIZE)
#define KEYMAP_SIZE_WORD GET_WORD(LAYER_SIZE* MAX_LAYER)
#define KEYMAP_RECORD_KEY 0x1905

// todo: 增加keymap是否可用的判定
#define USE_KEYMAP true

REGISTER_FDS_BLOCK(keymap, KEYMAP_SIZE_WORD, KEYMAP_RECORD_KEY)
#ifndef ACTIONMAP_ENABLE
uint8_t keymap_key_to_keycode(uint8_t layer, keypos_t key)
{
    if (layer >= LAYER_SIZE || key.col >= MATRIX_COLS || key.row >= MATRIX_ROWS)
        return KC_NO;
    if (USE_KEYMAP)
        return keymap_block[layer * LAYER_SIZE + key.row * MATRIX_COLS + key.col];
    else
        return keymaps[layer][key.row][key.col];
}
#else
extern const action_t actionmaps[][MATRIX_ROWS][MATRIX_COLS];

action_t action_for_key(uint8_t layer, keypos_t key) {
    if (layer >= LAYER_SIZE || key.col >= MATRIX_COLS || key.row >= MATRIX_ROWS) {
        action_t action = AC_NO;
        return action;
    }
    if (USE_KEYMAP) {
        action_t action;
        uint16_t index = (layer * LAYER_SIZE + key.row * MATRIX_COLS + key.col) * 2;
        action.code = ((uint16_t)keymap_block[index + 1] << 8) + keymap_block[index];
        return action;
    } else {
        return actionmaps[layer][key.row][key.col];
    }
}
#endif
#pragma endregion

#ifndef ACTIONMAP_ENABLE
#pragma region FN_MAP
#define FN_BLOCK_SIZE_WORD GET_WORD(MAX_FN_KEYS * 2)
#define FN_RECORD_KEY 0x1906

REGISTER_FDS_BLOCK(fn, FN_BLOCK_SIZE_WORD, FN_RECORD_KEY)

action_t keymap_fn_to_action(uint8_t keycode)
{
    if (USE_KEYMAP) {
        uint8_t index = FN_INDEX(keycode) * 2;
        uint16_t action = ((uint16_t)fn_block[index + 1] << 8) + fn_block[index];
        return (action_t)action;
    } else
        return fn_actions[FN_INDEX(keycode)];
}
#pragma endregion
#endif
#endif

#ifdef MARCO_STORAGE
#pragma region MACRO
#define MACRO_BLOCK_SIZE_WORD GET_WORD(MAX_MACRO_SIZE)
#define MACRO_RECORD_KEY 0x1907

REGISTER_FDS_BLOCK(macro, MACRO_BLOCK_SIZE_WORD, MACRO_RECORD_KEY)

const macro_t* action_get_macro(keyrecord_t* record, uint8_t id, uint8_t opt)
{
    if (!record->event.pressed)
        return MACRO_NONE;
    if (id == 0)
        return macro_block;

    uint16_t index = 0;
    while (index < MAX_MACRO_SIZE) {
        uint8_t code = macro_block[index++];
        if (code == END && --id == 0) {
            // 到达目标位置
            return &macro_block[index];
        }
        if (code == KEY_DOWN || code == KEY_UP || code == WAIT || code == INTERVAL)
            index++; // 2 byte command
    }

    return MACRO_NONE;
}
#endif
#pragma endregion

#ifdef CONFIG_STORAGE
#pragma region CONFIG
#define CONFIG_BLOCK_SIZE_WORD 4
#define CONFIG_RECORD_KEY 0x1908

REGISTER_FDS_BLOCK(config, CONFIG_BLOCK_SIZE_WORD, CONFIG_RECORD_KEY)
#pragma endregion
#endif

#pragma region FDS_INNER
/**
 * @brief 内部读取FDS的数据，如果数据不存在则尝试新建一个
 * 
 * @param record FDS记录
 * @param record_desc FDS记录描述
 */
static void storage_read_inner(fds_record_t const* record, fds_record_desc_t* record_desc)
{
    fds_find_token_t ftok = { 0 };
    fds_flash_record_t flash_record = { 0 };

    // 查找对应记录
    if (fds_record_find(record->file_id, record->key, record_desc, &ftok) == FDS_SUCCESS) {
        fds_record_open(record_desc, &flash_record);

        if (flash_record.p_header->length_words == record->data.length_words) {
            // 大小正常，读取数据
            memcpy(keymap_block, flash_record.p_data, record->data.length_words * 4);
            fds_record_close(record_desc);
        } else {
            // 大小不正常，尝试更新数据
            fds_record_close(record_desc);
            ret_code_t code = fds_record_update(record_desc, record);
            APP_ERROR_CHECK(code);
        }
    } else {
        // 记录不存在，尝试新建一个
        ret_code_t code = fds_record_write(record_desc, record);
        APP_ERROR_CHECK(code);
    }
}

/**
 * @brief 内部更新FDS的数据
 * 
 * @param record FDS 记录
 * @param record_desc FDS 记录描述。必须先调用 storage_read_inner 或其他函数生成此描述后才能调用此函数。
 */
static void storage_update_inner(fds_record_t const* record, fds_record_desc_t* record_desc)
{
    ret_code_t err_code = fds_record_update(record_desc, record);
    // 空间写满了，准备GC
    if (err_code == FDS_ERR_NO_SPACE_IN_FLASH) {
        err_code = fds_gc();
        // todo: 将操作入栈，等待gc完毕后重新操作
    }
}

#pragma endregion

/**
 * @brief 读取存储的记录并写到内存（重置内存中的数据）
 * 
 * @param type 要读取的记录mask。1: keymap, 2: fn, 4: macro, 8: config
 */
void storage_read(uint8_t type)
{
    if (type & 0x01) {
#ifdef KEYMAP_STORAGE
        storage_read_inner(&keymap_record, &keymap_record_desc);
#endif
    }
    if (type & 0x02) {
#if defined(KEYMAP_STORAGE) && !defined(ACTIONMAP_ENABLE)
        storage_read_inner(&fn_record, &fn_record_desc);
#endif
    }
    if (type & 0x04) {
#ifdef MARCO_STORAGE
        storage_read_inner(&macro_record, &macro_record_desc);
#endif
    }
    if (type & 0x08) {
#ifdef MARCO_STORAGE
        storage_read_inner(&config_record, &config_record_desc);
#endif
    }
}

/**
 * @brief 初始化存储模块（读取记录）
 * 
 */
void storage_init()
{
    storage_read(0xFF);
}

/**
 * @brief 写存储模块记录
 * 
 * @param type 要写出的记录mask。1: keymap, 2: fn, 4: macro, 8: config
 * @return true 操作成功
 * @return false 含有未定义的操作
 */
bool storage_write(uint8_t type)
{
    bool success = true;

    if (type & 0x01) {
#ifdef KEYMAP_STORAGE
        storage_update_inner(&keymap_record, &keymap_record_desc);
#endif
    }
    if (type & 0x02) {
#if defined(KEYMAP_STORAGE) && !defined(ACTIONMAP_ENABLE)
        storage_update_inner(&fn_record, &fn_record_desc);
#endif
    }
    if (type & 0x04) {
#ifdef MARCO_STORAGE
        storage_update_inner(&macro_record, &macro_record_desc);
#endif
    }
    if (type & 0x08) {
#ifdef MARCO_STORAGE
        storage_update_inner(&config_record, &config_record_desc);
#endif
    }

    return success;
}

/**
 * @brief 获取指定类型的内存区域的指针和大小
 * 
 * @param type 
 * @param pointer 
 * @return uint16_t 
 */
static uint16_t storage_get_data_pointer(uint8_t type, uint8_t** pointer)
{
    switch (type) {
#ifdef KEYMAP_STORAGE
    case 0:
        pointer = keymap_block;
        return KEYMAP_SIZE_WORD * 4;
        break;
#endif
#if defined(KEYMAP_STORAGE) && !defined(ACTIONMAP_ENABLE)
    case 1:
        pointer = fn_block;
        return FN_BLOCK_SIZE_WORD * 4;
        break;
#endif
#ifdef MARCO_STORAGE
    case 2:
        pointer = macro_block;
        return MACRO_BLOCK_SIZE_WORD * 4;
        break;
#endif
#ifdef MARCO_STORAGE
    case 3:
        pointer = config_block;
        return CONFIG_BLOCK_SIZE_WORD * 4;
        break;
#endif
    default:
        return 0;
    }
}

/**
 * @brief 读取内存中的数据
 * 
 * @param type 数据类型 0: keymap, 1: fn, 2: macro, 3: config
 * @param offset 数据偏移量
 * @param len 数据长度
 * @param data 目标指针
 * @return uint16_t 读取实际长度
 */
uint16_t storage_read_data(uint8_t type, uint16_t offset, uint16_t len, uint8_t* data)
{
    uint8_t* pointer;
    uint16_t size = storage_get_data_pointer(type, &pointer);

    if (size < len + offset)
        len = size - offset;

    memcpy(data, &pointer[offset], len);
    return len;
}

/**
 * @brief 将数据写到内存中
 * 
 * @param type 数据类型 0: keymap, 1: fn, 2: macro, 3: config
 * @param offset 数据偏移量
 * @param len 数据长度
 * @param data 数据指针
 * @return uint16_t 实际写入长度
 */
uint16_t storage_write_data(uint8_t type, uint16_t offset, uint16_t len, uint8_t* data)
{
    uint8_t* pointer;
    uint16_t size = storage_get_data_pointer(type, &pointer);

    if (size < len + offset)
        len = size - offset;

    memcpy(&pointer[offset], data, len);
    return len;
}