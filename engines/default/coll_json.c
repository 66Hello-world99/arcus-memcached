#include "config.h"
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <sched.h>
#include <inttypes.h>

/* Dummy PERSISTENCE_ACTION Macros */
#define PERSISTENCE_ACTION_BEGIN(a, b)
#define PERSISTENCE_ACTION_END(a)

#include "default_engine.h"
#include "item_clog.h"

static struct default_engine *engine=NULL;
static struct engine_config  *config=NULL; // engine config
static EXTENSION_LOGGER_DESCRIPTOR *logger;

/*
 * JSON collection management
 */
static ENGINE_ERROR_CODE do_json_item_find(struct default_engine *engine,
                                           const void *key, const size_t nkey,
                                           bool do_update, hash_item **item)
{
    *item = NULL;
    hash_item *it = do_item_get(key, nkey, do_update);
    if (it == NULL) {
        return ENGINE_KEY_ENOENT;
    }
    if (IS_JSON_ITEM(it)) {
        *item = it;
        return ENGINE_SUCCESS;
    } else {
        do_item_release(it);
        return ENGINE_EBADTYPE;
    }
}

static int32_t do_json_real_maxcount(int32_t maxcount)
{
    int32_t real_maxcount = maxcount;

    if (maxcount < 0) {
        /* It has the max_map_size that can be increased in the future */
        real_maxcount = -1;
    } else if (maxcount == 0) {
        real_maxcount = DEFAULT_JSON_SIZE;
    } else if (maxcount > config->max_map_size) {
        real_maxcount = config->max_map_size;
    }
    return real_maxcount;
}

static hash_item *do_json_item_alloc(struct default_engine *engine,
                                     const void *key, const size_t nkey,
                                     item_attr *attrp, const void *cookie)
{   //todo delete?
    uint32_t flags = (attrp != NULL) ? attrp->flags : 0;
    rel_time_t exptime = (attrp != NULL) ? attrp->exptime : 0;

    uint32_t nbytes = 2; /* "\r\n" */
    int real_nbytes = META_OFFSET_IN_ITEM(nkey,nbytes) 
                     + sizeof(json_meta_info) - nkey;

    //hash_item *it = do_item_alloc(key, nkey, attrp->flags, attrp->exptime,
    //                              real_nbytes, cookie); 그냥 attrp 들어가는거 다 확인 todo
    hash_item *it = do_item_alloc(key, nkey, flags, exptime,
                                  real_nbytes, cookie);

    if (it != NULL) {
        it->iflag |= ITEM_IFLAG_JSON;
        it->nbytes = nbytes; /* NOT real_nbytes */
        memcpy(item_get_data(it), "\r\n", nbytes);

        /* initialize json meta information */
        json_meta_info *info = (json_meta_info *)item_get_meta(it);
        //info->mcnt = do_json_real_maxcount(attrp->maxcount);
        info->mcnt = do_json_real_maxcount((attrp != NULL) ? attrp->maxcount : 0);
        info->ccnt = 0;
        info->ovflact = OVFL_ERROR;
        info->mflags = 0;
#ifdef ENABLE_STICKY_ITEM
        if (attrp != NULL && IS_STICKY_EXPTIME(attrp->exptime)){ 
            info->mflags |= COLL_META_FLAG_STICKY;
        }
#endif
        if (attrp != NULL && attrp->readable == 1) {
            info->mflags |= COLL_META_FLAG_READABLE;
        }
        info->itdist = (uint16_t)((size_t*)info-(size_t*)it);
        info->stotal = 0;
        info->root    = NULL;
        assert((hash_item*)COLL_GET_HASH_ITEM(info) == it);
    }
    return it;
}

static json_elem_item *do_json_elem_alloc(struct default_engine *engine, json_node_type type,
                                          json_value *value, const void *cookie)
{
    json_elem_item *elem_item = NULL;

    switch(type) {
      case N_DICT:
           elem_item = new_dict_item(value->intval);
           break;
      case N_ARRAY:
           elem_item = new_array_item(value->intval);
           break;
      case N_STRING:
           elem_item = new_string_item(value->strval.pos, value->strval.len);
           break;
      case N_KEYVAL:
           elem_item = new_keyval_item(value->strval.pos, value->strval.len, NULL);
           break;
      case N_NUMBER:
           elem_item = new_double_item(value->numval);
           break;
      case N_INTEGER:
           elem_item = new_int_item(value->intval);
           break;
      case N_BOOLEAN:
           elem_item = new_bool_item(value->boolval);
           break;
      case N_NULL:
           elem_item = new_null_item();
           break;
    }
    return elem_item;
}

static void do_json_elem_free(json_elem_item **elem)
{
    json_elem_item *_elem = *elem;

    if (_elem == NULL) return ;

    switch (_elem->type) {
      case N_ARRAY:
           free(_elem->value.arrval.entries);
           _elem->value.arrval.entries = NULL;
           break;
      case N_DICT:
           free(_elem->value.dictval.entries);
           _elem->value.dictval.entries = NULL;
           break;
      case N_KEYVAL:
           free((char*)_elem->value.kvval.key);
           _elem->value.kvval.key = NULL;
           break;
      case N_STRING:
           free((char*)_elem->value.strval.data);
           _elem->value.strval.data = NULL;
           break;
      case N_NULL:
      case N_NUMBER:
      case N_INTEGER:
      case N_BOOLEAN:
           break;
    }
    free(_elem);
    *elem = NULL;
}

static void do_json_elem_release(json_elem_item **elem)
{
    json_elem_item *_elem = *elem;
    json_elem_item **container;
    int i, len;

    if (_elem == NULL) return ;

    if (_elem->refcount != 0) {
        _elem->refcount--;
    }

    switch (_elem->type) {
      case N_ARRAY:
      case N_DICT:
           if (_elem->type == N_ARRAY) {
               container = _elem->value.arrval.entries;
               len = _elem->value.arrval.len;
           } else {
               container = _elem->value.dictval.entries;
               len = _elem->value.dictval.len;
           }

           if (container != NULL) {
               for (i = 0; i < len; i++) {
                   do_json_elem_release(&container[i]);
               }
           }
           break;
      case N_KEYVAL:
           do_json_elem_release(&_elem->value.kvval.val);
           break;
      default:
           break;
    }

    if (_elem->refcount == 0 && _elem->status == JSON_ITEM_STATUS_UNLINK) {
        _elem->status = JSON_ITEM_STATUS_FREE;
        do_json_elem_free(elem);
    }
}

static ENGINE_ERROR_CODE do_json_elem_get(json_path_node *jpn, json_elem_item *root,
                                          const char *path, const size_t npath)
{
    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;
    jpn->sp = new_search_path(0);
    jpn->spath = path;
    jpn->spathlen = npath;
    if (npath == 1 && path[0] == '$') {
    jpn->sp.len = 0;
    }//char*로 받아온 패스값을 파싱하여 jpn의 sp에 저장
    else if (parse_json_path(jpn->spath, jpn->spathlen, &jpn->sp) == PARSE_ERR) {
        ret = ENGINE_EBADVALUE;
    } else ret = ENGINE_SUCCESS;

    if (jpn == NULL) ret = ENGINE_EBADVALUE;

    do {
        if (ret != ENGINE_SUCCESS) break;

        if (!search_path_is_root_path(&jpn->sp)) {
            if (search_path_find_ex(&jpn->sp, root, &jpn->n, &jpn->p) !=  ENGINE_SUCCESS) {
                ret = ENGINE_EBADVALUE;
            }
        } else {
            jpn->p = NULL;
            jpn->n = root;
        }
    } while(0);
    return ret;
}

static void do_json_elem_unlink(json_elem_item **elem)
{
    json_elem_item *_elem = *elem;
    json_elem_item **container = NULL;
    int i, len;

    if (_elem == NULL) return ;

    _elem->status = JSON_ITEM_STATUS_UNLINK;

    switch (_elem->type) {
      case N_ARRAY:
      case N_DICT:
           if (_elem->type == N_ARRAY) {
               container = _elem->value.arrval.entries;
               len = _elem->value.arrval.len;
           } else {
               container = _elem->value.dictval.entries;
               len = _elem->value.dictval.len;
           }

           if (container != NULL) {
               for (i = 0; i < len; i++) {
                   do_json_elem_unlink(&container[i]);
               }
           }
           break;
      case N_KEYVAL:
           do_json_elem_unlink(&_elem->value.kvval.val);
           break;
      default:
           break;
    }

    if (_elem->refcount == 0) {
        _elem->status = JSON_ITEM_STATUS_FREE;
        do_json_elem_free(elem);
    }
}

static void do_json_elem_link(json_elem_item *elem)
{
    json_elem_item **container = NULL;
    int i, len;

    if (elem == NULL) return ;

    elem->refcount++;
    elem->status = JSON_ITEM_STATUS_USED;

    switch (elem->type) {
      case N_ARRAY:
      case N_DICT:
           if (elem->type == N_ARRAY) {
               container = elem->value.arrval.entries;
               len = elem->value.arrval.len;
           } else {
               container = elem->value.dictval.entries;
               len = elem->value.dictval.len;
           }

           if (container != NULL) {
               for (i = 0; i < len; i++) {
                   do_json_elem_link(container[i]);
               }
           }
           break;
      case N_KEYVAL:
           do_json_elem_link(elem->value.kvval.val);
           break;
      default:
           break;
    }
}

static ENGINE_ERROR_CODE do_json_elem_append(struct default_engine *engine, json_elem_item **dest,
                                             json_elem_item *e, json_elem_item *e_temp,
                                             json_node_type type, const void *cookie)
{
    json_elem_item *old = NULL;
    ENGINE_ERROR_CODE ret;

    switch(type) {
    case N_DICT:
        ret = item_dict_set_keyval(*dest, e, &old);
        if (old != NULL) do_json_elem_unlink(&old);
        break;
    case N_ARRAY:
        ret = item_array_append(*dest, e);
        break;
    case N_KEYVAL:
        e->value.kvval.val = e_temp;
        ret = item_dict_set_keyval(*dest, e, &old);
        if (old != NULL) do_json_elem_unlink(&old);
        break;
    default:
        ret = ENGINE_ENOMEM;
        break;
    }
    return ret;
}

static ENGINE_ERROR_CODE do_json_elem_set(struct default_engine *engine,
                                          hash_item *it, json_elem_item *elem_item,
                                          const char *path, const size_t npath)
{
    json_path_node jpn;
    ENGINE_ERROR_CODE ret;
    json_meta_info *info = (json_meta_info *)item_get_meta(it);

    if (npath == 1 && path[0] == '$') {
        if (info->root != NULL) do_json_elem_unlink(&info->root);
        info->root = elem_item;
        return ENGINE_SUCCESS;
    }

    
        ret = do_json_elem_get(&jpn, info->root, path, npath);
        if(ret == ENGINE_SUCCESS) {
            bool delete_old_elem = true;

            if (search_path_is_root_path(&jpn.sp)) {
                if (info->root != NULL && info->root != elem_item) {
                    do_json_elem_unlink(&info->root); 
                }
                info->root = elem_item;
            } else if(jpn.p !=NULL){
                if (jpn.p->type == N_ARRAY) {
                int index = jpn.sp.nodes[jpn.sp.len - 1].value.index;
                if (item_array_set(jpn.p, index, elem_item) != ENGINE_SUCCESS) {
                    delete_old_elem = false;
                    ret = ENGINE_ENOMEM;
                }
            } else if (jpn.p->type == N_DICT) {
                const char *tmp_key = jpn.sp.nodes[jpn.sp.len - 1].value.key;
                if (item_dict_set(jpn.p, tmp_key, elem_item) != ENGINE_SUCCESS) {
                    delete_old_elem = false;
                    ret = ENGINE_ENOMEM;
                }
            } else {
                ret = ENGINE_ENOMEM;
            }
            } 
            if (jpn.n != NULL && jpn.n != elem_item && delete_old_elem) {
                do_json_elem_unlink(&jpn.n);
            }
        }
    
    return ret;
}

static ENGINE_ERROR_CODE do_json_elem_insert_buffer(size_t *offset, json_get_elem *get_elem, char *data,
                                                    size_t data_len, uint32_t indent)
{
    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;
    int i;
    do {
        size_t new_len = *offset + data_len + indent * 2;
        if (new_len > get_elem->cap) {
            while(new_len > get_elem->cap) {
                if (get_elem->cap * 2  == 0) {  //overflow ck
                    ret = ENGINE_E2BIG; break;
                }
                get_elem->cap *= 2;
            }
            if (ret != ENGINE_SUCCESS) break;

            char *temp_buffer = (char*)realloc(get_elem->buffer, sizeof(char) * get_elem->cap);
            if (temp_buffer != NULL) get_elem->buffer = temp_buffer;
            else { ret = ENGINE_ENOMEM; break; }
        }
        char *cur = get_elem->buffer + *offset;
        char *input_data = (char*)malloc((data_len + indent * 2 + 1)*sizeof(char));
        size_t space_offset = 0;
        for (i=0; i<indent; i++) {
            if (sprintf(input_data + space_offset, "  ") < 0) {
                ret = ENGINE_ENOMEM; break;
            }
            space_offset += 2;
        }

        if (ret == ENGINE_SUCCESS && sprintf(input_data + space_offset, "%s", data) < 0) {
            ret = ENGINE_ENOMEM;
        }

        if (ret == ENGINE_SUCCESS && sprintf(cur, "%s",input_data) < 0) {
            ret = ENGINE_ENOMEM;
        }
        *offset += data_len + indent * 2 ;
        free(input_data);
    } while(0);

    return ret;
}

static ENGINE_ERROR_CODE do_json_elem_dfs_buffer(json_elem_item *elem, const uint32_t indent,
                                                 size_t *offset, json_get_elem *get_elem)
{
    int i, len, data_len;
    int64_t int_tmp;
    json_elem_item **cur;
    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;
    char *data = NULL;

    if (elem == NULL) {
        data = "deleted,\r\n";
        data_len = 10;
        ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len, indent);
        return ret;
    }

    elem->refcount++;

    switch (elem->type) {
      case N_ARRAY:
      case N_DICT:
           if (elem->type == N_ARRAY) {
               cur = elem->value.arrval.entries;
               len = elem->value.arrval.len;
               data = "[\r\n";
           } else {
               cur = elem->value.dictval.entries;
               len = elem->value.dictval.len;
               data = "{\r\n";
           }
           data_len = 3;
           ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len, indent);
           if (ret != ENGINE_SUCCESS) break;
           if (cur != NULL) {
               for (i = 0; i < len; i++) {
                   ret = do_json_elem_dfs_buffer(cur[i], indent+1, offset, get_elem);
               }
               if (len != 0) {
                   *offset -= 3;
                   data = "\r\n";
                   data_len = 2;
                   ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len, 0);
                   if (ret != ENGINE_SUCCESS) break;
               }
           } else {
               data = "deleted,\r\n";
               data_len = 10;
               ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len, indent+1);
               if (ret != ENGINE_SUCCESS) break;
           }
           data = elem->type == N_ARRAY ? "],\r\n" : "},\r\n";
           data_len = 4;

           ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len, indent);
           break;
      case N_KEYVAL:
           data_len = strlen(elem->value.kvval.key) + 3;
           data = (char*)malloc((data_len + 1)*sizeof(char));
           sprintf(data,"%s:\r\n",elem->value.kvval.key);
           ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len, indent);
           if (ret != ENGINE_SUCCESS) break;
           ret = do_json_elem_dfs_buffer(elem->value.kvval.val, indent+1, offset, get_elem);
           free(data);
           break;
      case N_STRING:
           data_len = elem->value.strval.len + 3;
           data = (char*)malloc((data_len + 1) * sizeof(char));
           strcpy(data, elem->value.strval.data);
           strcat(data, ",\r\n");
           ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len, indent);
           free(data);
           break;
      case N_NULL:
           data = "null,\r\n";
           data_len = 7;
           ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len, indent);
           break;
      case N_NUMBER:
           data = (char*)malloc(25 * sizeof(char));
           gcvt(elem->value.numval, 17, data);
           data_len = strlen(data);
           sprintf(data + data_len, ",\r\n");
           ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len + 3, indent);
           free(data);
           break;
      case N_INTEGER:
           int_tmp = elem->value.intval;
           data_len = 3;
           if (int_tmp == 0) data_len += 1;
           else if (int_tmp < 0) { int_tmp = -int_tmp; data_len += 1; }
           while(int_tmp > 0) {
               data_len += 1;
               int_tmp /= 10;
           }
           data = (char*)malloc((data_len + 1)*sizeof(char));
           sprintf(data, "%lld,\r\n", elem->value.intval);
           ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len, indent);
           free(data);
           break;
      case N_BOOLEAN:
           data = elem->value.boolval ? "true,\r\n" : "false,\r\n";
           data_len = elem->value.boolval ? 7 : 8;
           ret = do_json_elem_insert_buffer(offset, get_elem, data, data_len, indent);
           break;
    }
    return ret;
}

/* Cache Lock */
static inline void LOCK_CACHE(void)
{
    pthread_mutex_lock(&engine->cache_lock);
}

static inline void UNLOCK_CACHE(void)
{
    pthread_mutex_unlock(&engine->cache_lock);
}

ENGINE_ERROR_CODE json_struct_create(const char *key, const size_t nkey,
                                     item_attr *attrp, const void *cookie)
{
    ENGINE_ERROR_CODE ret;
    hash_item *it;
    PERSISTENCE_ACTION_BEGIN(cookie, UPD_JSON_CREATE);

    LOCK_CACHE();
    it = do_item_get(key, nkey, DONT_UPDATE);
    if (it != NULL) {
        do_item_release(it);
        ret = ENGINE_KEY_EEXISTS;
    } else {
        it = do_json_item_alloc(engine, key, nkey, attrp, cookie);
        if (it == NULL) {
            ret = ENGINE_ENOMEM;
        } else {
            ret = do_item_link(it);
            do_item_release(it);
        }
    }
    UNLOCK_CACHE();

    PERSISTENCE_ACTION_END(ret);
    return ret;
}

json_elem_item *json_elem_alloc(struct default_engine *engine, json_node_type type,
                                json_value *value, const void *cookie)
{
    json_elem_item *elem;
    LOCK_CACHE();
    elem = do_json_elem_alloc(engine, type, value, cookie);
    UNLOCK_CACHE();
    return elem;
}

ENGINE_ERROR_CODE json_elem_append(struct default_engine *engine, json_elem_item **dest,
                                   json_elem_item *e, json_elem_item *e_temp,
                                   json_node_type type, const void *cookie)
{
    ENGINE_ERROR_CODE ret;
    LOCK_CACHE();
    ret = do_json_elem_append(engine, dest, e, e_temp, type, cookie);
    UNLOCK_CACHE();
    return ret;
}

ENGINE_ERROR_CODE json_elem_set(struct default_engine *engine,
                                const char *key, const size_t nkey,
                                const char *path, const size_t npath,
                                json_elem_item *elem_item, item_attr *attrp,
                                bool *created, const void *cookie)
{
    hash_item *it = NULL;
    ENGINE_ERROR_CODE ret;
    *created = false;
    
    LOCK_CACHE();
    ret = do_json_item_find(engine, key, nkey, DONT_UPDATE, &it);
    if (ret == ENGINE_KEY_ENOENT) {
        it = do_json_item_alloc(engine, key, nkey, attrp, cookie);
        if (it == NULL) {
            ret = ENGINE_ENOMEM;
        } else {
            ret = do_item_link(it);
            if (ret == ENGINE_SUCCESS) {
                *created = true;
            } else {
                /* The item is to be released, below */
            }
        }
    }
    if (ret == ENGINE_SUCCESS) {
        ret = do_json_elem_set(engine, it, elem_item, path, npath);
        do_json_elem_link(elem_item);
        if (ret != ENGINE_SUCCESS && *created) {
            do_item_unlink(it, ITEM_UNLINK_NORMAL);
        }
    }
    if (it != NULL) do_item_release(it);
    UNLOCK_CACHE();
    return ret;
}

ENGINE_ERROR_CODE json_elem_delete(struct default_engine *engine,
                                   const char *key, const size_t nkey,
                                   const char *path, const size_t npath,
                                   const bool drop_if_empty, bool *dropped)
{
    hash_item *it;
    json_path_node jpn;
    ENGINE_ERROR_CODE ret;

    memset(&jpn, 0, sizeof(json_path_node));

    LOCK_CACHE();
    ret = do_json_item_find(engine, key, nkey, DONT_UPDATE, &it);
    if (ret == ENGINE_SUCCESS) {
        json_meta_info *info = (json_meta_info*)item_get_meta(it);
        ret = do_json_elem_get(&jpn, info->root, path, npath);

        if (ret == ENGINE_SUCCESS) {
            if (jpn.p!=NULL) {
                int index=-1;
                json_elem_item **elem_item = NULL;

                if (jpn.p->type == N_ARRAY) {
                    index = jpn.sp.nodes[jpn.sp.len - 1].value.index;
                    if (index >= 0 && index < (int)jpn.p->value.arrval.len) {
                        elem_item = &jpn.p->value.arrval.entries[index];
                    }
                } else {
                    size_t dict_len = jpn.p->value.dictval.len;
                    const char *dict_key = jpn.sp.nodes[jpn.sp.len - 1].value.key;
                    json_elem_item **dict_entries = jpn.p->value.dictval.entries;
                    for (int i = 0; i < dict_len; i++) {
                        if (strcmp(dict_entries[i]->value.kvval.key, dict_key) == 0) {
                            index = i;
                            elem_item = &dict_entries[i];
                            break; 
                        }
                    }
                }

                if (elem_item != NULL && *elem_item != NULL) {
                    do_json_elem_unlink(elem_item);
                    
                    // parents node type is array
                    if (jpn.p->type == N_ARRAY) {
                        size_t new_len = --jpn.p->value.arrval.len;
                        json_elem_item **entries = jpn.p->value.arrval.entries;
                        if (index < (int)new_len) {
                            memmove(&entries[index], &entries[index + 1], sizeof(json_elem_item *) * (new_len - index));
                        }
                        entries[new_len] = NULL; // 마지막 포인터 초기화
                    } else {
                        // parents node type is dictionary
                        size_t new_len = --jpn.p->value.dictval.len;
                        json_elem_item **entries = jpn.p->value.dictval.entries;
                        if (index < (int)new_len) {
                            entries[index] = entries[new_len];
                        }
                        entries[new_len] = NULL;
                    }
                } else {
                    ret = ENGINE_ELEM_ENOENT;
                }
            } else {
                //delete root($)
                if (info->root != NULL) {
                    do_json_elem_unlink(&info->root);
                    info->root = NULL; 
                } else {
                    ret = ENGINE_ELEM_ENOENT;
                }
            }

            if (ret == ENGINE_SUCCESS) {
                if (info->root == NULL && drop_if_empty) {
                    do_item_unlink(it, ITEM_UNLINK_NORMAL);
                    *dropped = true;
                } else {
                    *dropped = false;
                }
            }
            if (it != NULL) {
                do_item_release(it);
            }
        }
    }
    UNLOCK_CACHE();
    return ret;
}

ENGINE_ERROR_CODE json_elem_get(struct default_engine *engine,
                                const char *key, const size_t nkey,
                                const char *path, const size_t npath,
                                json_elem_item **elem)
{
    hash_item *it = NULL;
    ENGINE_ERROR_CODE ret;
    json_path_node jpn;

    LOCK_CACHE();
    ret = do_json_item_find(engine, key, nkey, DONT_UPDATE, &it);//전체 해시맵에서 json 타입인 해시 아이템의 찾아옴
    if (ret == ENGINE_SUCCESS) {
        json_meta_info *info = (json_meta_info *)item_get_meta(it); //그 키 뒤에 붙어있는 메타정보(트리정보) 가져옴
        ret = do_json_elem_get(&jpn, info->root, path, npath); //path 경로를 따라 루트부터 타겟노드를 찾아옴
        if (ret == ENGINE_SUCCESS) {
            *elem = jpn.n; //최종적으로 찾아낸 타겟노드의 포인터 반환
        }
    }
    UNLOCK_CACHE();

    return ret;
}

ENGINE_ERROR_CODE json_elem_dfs(eitem *elem, char **buffer, size_t *len)
{
    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;
    size_t offset = *len;
    json_get_elem *get_elem = (json_get_elem*)malloc(sizeof(json_get_elem));
    get_elem->cap = 256;

    while(*len > get_elem->cap) {
        if (get_elem->cap * 2 == 0) {
            ret = ENGINE_E2BIG; break;
        }
        get_elem->cap *= 2;
    }
    do {
        if (ret == ENGINE_E2BIG) break;

        char *temp_buffer = (char*)realloc(*buffer , get_elem->cap * sizeof(char));
        if (temp_buffer != NULL) get_elem->buffer = temp_buffer;
        else { ret = ENGINE_ENOMEM; break; }

        ret = do_json_elem_dfs_buffer(elem, 0, &offset, get_elem);

        if (ret != ENGINE_SUCCESS) break;

        offset -= 3;
        ret = do_json_elem_insert_buffer(&offset, get_elem, "\r\nEND\r\n", 7, 0);

        *len = offset;
        *buffer = get_elem->buffer;
    } while(0);
    free(get_elem);

    return ret;
}

void json_elem_release(struct default_engine *engine, json_elem_item **e)
{
    LOCK_CACHE();
    do_json_elem_release(e);
    UNLOCK_CACHE();
}

void json_elem_scalar(struct default_engine *engine, json_elem_item **e, json_elem_item **dest)
{
    LOCK_CACHE();
    item_array_item(*e, 0, dest);
    item_array_set(*e, 0, NULL);
    do_json_elem_release(e);
    UNLOCK_CACHE();
}

/*
 * External Functions
 */
ENGINE_ERROR_CODE item_json_coll_init(void *engine_ptr)
{
    /* initialize global variables */
    engine = engine_ptr;
    config = &engine->config;
    logger = engine->server.log->get_logger();

    logger->log(EXTENSION_LOG_INFO, NULL, "ITEM json module initialized.\n");
    return ENGINE_SUCCESS;
}

void item_json_coll_final(void *engine_ptr)
{
    logger->log(EXTENSION_LOG_INFO, NULL, "ITEM json module destroyed.\n");
}
