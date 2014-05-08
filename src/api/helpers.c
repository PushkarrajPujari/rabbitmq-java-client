#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "api/helpers.h"
#include "api/defs.h"
#include "../vim.h"
#include "memory.h"
#include "eval.h"

#include "lib/khash.h"

KHASH_SET_INIT_INT64(Lookup)

/// Recursion helper for the `vim_to_object`. This uses a pointer table
/// to avoid infinite recursion due to cyclic references
///
/// @param obj The source object
/// @param lookup Lookup table containing pointers to all processed objects
/// @return The converted value
static Object vim_to_object_rec(typval_T *obj, khash_t(Lookup) *lookup);

static bool object_to_vim(Object obj, typval_T *tv, Error *err);

void try_start()
{
  ++trylevel;
}

bool try_end(Error *err)
{
  --trylevel;

  // Without this it stops processing all subsequent VimL commands and
  // generates strange error messages if I e.g. try calling Test() in a
  // cycle
  did_emsg = false;

  if (got_int) {
    if (did_throw) {
      // If we got an interrupt, discard the current exception 
      discard_current_exception();
    }

    set_api_error("Keyboard interrupt", err);
    got_int = false;
  } else if (msg_list != NULL && *msg_list != NULL) {
    int should_free;
    char *msg = (char *)get_exception_string(*msg_list,
                                             ET_ERROR,
                                             NULL,
                                             &should_free);
    strncpy(err->msg, msg, sizeof(err->msg));
    err->set = true;
    free_global_msglist();

    if (should_free) {
      free(msg);
    }
  } else if (did_throw) {
    set_api_error((char *)current_exception->value, err);
  }

  return err->set;
}

Object dict_get_value(dict_T *dict, String key, bool pop, Error *err)
{
  Object rv;
  hashitem_T *hi;
  dictitem_T *di;
  char k[key.size + 1];
  // Convert the key
  memcpy(k, key.data, key.size);
  k[key.size] = NUL;
  hi = hash_find(&dict->dv_hashtab, (uint8_t *)k);

  if (HASHITEM_EMPTY(hi)) {
    set_api_error("Key not found", err);
    return rv;
  }

  di = dict_lookup(hi);
  rv = vim_to_object(&di->di_tv);

  if (pop) {
    if (dict->dv_lock) {
      set_api_error("Dictionary is locked", err);
      return rv;
    }

    hash_remove(&dict->dv_hashtab, hi);
    dictitem_free(di);
  }

  return rv;
}

Object dict_set_value(dict_T *dict, String key, Object value, Error *err)
{
  Object rv = {.type = kObjectTypeNil};

  if (dict->dv_lock) {
    set_api_error("Dictionary is locked", err);
    return rv;
  }

  if (key.size == 0) {
    set_api_error("Empty dictionary keys aren't allowed", err);
    return rv;
  }

  dictitem_T *di = dict_find(dict, (uint8_t *)key.data, key.size);
  typval_T tv;

  if (!object_to_vim(value, &tv, err)) {
    return rv;
  }

  if (di == NULL) {
    uint8_t k[key.size + 1];
    memcpy(k, key.data, key.size);
    k[key.size] = NUL;
    di = dictitem_alloc(k);
    dict_add(dict, di);
  } else {
    rv = vim_to_object(&di->di_tv);
    clear_tv(&di->di_tv);
  }

  copy_tv(&tv, &di->di_tv);
  clear_tv(&tv);

  return rv;
}

Object vim_to_object(typval_T *obj)
{
  Object rv;
  // We use a lookup table to break out of cyclic references 
  khash_t(Lookup) *lookup = kh_init(Lookup);
  rv = vim_to_object_rec(obj, lookup);
  // Free the table
  kh_destroy(Lookup, lookup);
  return rv;
}

static bool object_to_vim(Object obj, typval_T *tv, Error *err)
{
  tv->v_type = VAR_UNKNOWN;
  tv->v_lock = 0;

  switch (obj.type) {
    case kObjectTypeNil:
      tv->v_type = VAR_NUMBER;
      tv->vval.v_number = 0;
      break;

    case kObjectTypeBool:
      tv->v_type = VAR_NUMBER;
      tv->vval.v_number = obj.data.boolean;
      break;

    case kObjectTypeInt:
      tv->v_type = VAR_NUMBER;
      tv->vval.v_number = obj.data.integer;
      break;

    case kObjectTypeFloat:
      tv->v_type = VAR_FLOAT;
      tv->vval.v_float = obj.data.floating_point;
      break;

    case kObjectTypeString:
      tv->v_type = VAR_STRING;
      tv->vval.v_string = (uint8_t *)xstrndup(obj.data.string.data,
                                              obj.data.string.size);
      break;

    case kObjectTypeArray:
      tv->v_type = VAR_LIST;
      tv->vval.v_list = list_alloc();

      for (uint32_t i = 0; i < obj.data.array.size; i++) {
        Object item = obj.data.array.items[i];
        listitem_T *li = listitem_alloc();

        if (!object_to_vim(item, &li->li_tv, err)) {
          // cleanup
          listitem_free(li);
          list_free(tv->vval.v_list, true);
          return false;
        }

        list_append(tv->vval.v_list, li);
      }
      tv->vval.v_list->lv_refcount++;
      break;

    case kObjectTypeDictionary:
      tv->v_type = VAR_DICT;
      tv->vval.v_dict = dict_alloc();

      for (uint32_t i = 0; i < obj.data.dictionary.size; i++) {
        KeyValuePair item = obj.data.dictionary.items[i];
        String key = item.key;

        if (key.size == 0) {
          set_api_error("Empty dictionary keys aren't allowed", err);
          // cleanup
          dict_free(tv->vval.v_dict, true);
          return false;
        }

        char k[key.size + 1];
        memcpy(k, key.data, key.size);
        k[key.size] = NUL;
        dictitem_T *di = dictitem_alloc((uint8_t *)k);

        if (!object_to_vim(item.value, &di->di_tv, err)) {
          // cleanup
          dictitem_free(di);
          dict_free(tv->vval.v_dict, true);
          return false;
        }

        dict_add(tv->vval.v_dict, di);
      }
      tv->vval.v_dict->dv_refcount++;
      break;
  }

  return true;
}

static Object vim_to_object_rec(typval_T *obj, khash_t(Lookup) *lookup)
{
  Object rv = {.type = kObjectTypeNil};

  if (obj->v_type == VAR_LIST || obj->v_type == VAR_DICT) {
    int ret;
    // Container object, add it to the lookup table
    kh_put(Lookup, lookup, (uint64_t)obj, &ret);
    if (!ret) {
      // It's already present, meaning we alredy processed it so just return
      // nil instead.
      return rv;
    }
  }

  switch (obj->v_type) {
    case VAR_STRING:
      if (obj->vval.v_string != NULL) {
        rv.type = kObjectTypeString;
        rv.data.string.data = xstrdup((char *)obj->vval.v_string);
        rv.data.string.size = strlen(rv.data.string.data);
      }
      break;

    case VAR_NUMBER:
      rv.type = kObjectTypeInt;
      rv.data.integer = obj->vval.v_number;
      break;

    case VAR_FLOAT:
      rv.type = kObjectTypeFloat;
      rv.data.floating_point = obj->vval.v_float;
      break;

    case VAR_LIST:
      {
        list_T *list = obj->vval.v_list;
        listitem_T *item;

        if (list != NULL) {
          rv.type = kObjectTypeArray;
          rv.data.array.size = list->lv_len;
          rv.data.array.items = xmalloc(list->lv_len * sizeof(Object));

          uint32_t i = 0;
          for (item = list->lv_first; item != NULL; item = item->li_next) {
            rv.data.array.items[i] = vim_to_object_rec(&item->li_tv, lookup);
            i++;
          }
        }
      }
      break;

    case VAR_DICT:
      {
        dict_T *dict = obj->vval.v_dict;
        hashtab_T *ht;
        uint64_t todo;
        hashitem_T *hi;
        dictitem_T *di;

        if (dict != NULL) {
          ht = &obj->vval.v_dict->dv_hashtab;
          todo = ht->ht_used;
          rv.type = kObjectTypeDictionary;

          // Count items
          rv.data.dictionary.size = 0;
          for (hi = ht->ht_array; todo > 0; ++hi) {
            if (!HASHITEM_EMPTY(hi)) {
              todo--;
              rv.data.dictionary.size++;
            }
          }

          rv.data.dictionary.items =
            xmalloc(rv.data.dictionary.size * sizeof(KeyValuePair));
          todo = ht->ht_used;
          uint32_t i = 0;

          // Convert all
          for (hi = ht->ht_array; todo > 0; ++hi) {
            if (!HASHITEM_EMPTY(hi)) {
              di = dict_lookup(hi);
              // Convert key
              rv.data.dictionary.items[i].key.data =
                xstrdup((char *)hi->hi_key);
              rv.data.dictionary.items[i].key.size =
                strlen((char *)hi->hi_key);
              // Convert value
              rv.data.dictionary.items[i].value =
                vim_to_object_rec(&di->di_tv, lookup);
              todo--;
              i++;
            }
          }
        }
      }
      break;
  }

  return rv;
}
