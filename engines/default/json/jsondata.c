#include "jsondata.h"

static char *rmstrndup(const char *s, size_t len)
{
    char *ret = (char*)malloc((len + 1) * sizeof(char));
    if (ret) {
        memcpy(ret, s, len);
    }
    ret[len] = '\0';
    return ret;
}

static json_elem_item *_new_item(json_node_type t)
{
    json_elem_item *ret = NULL;
    ret = (json_elem_item*)malloc(sizeof(json_elem_item));
    ret->type = t;
    ret->status = JSON_ITEM_STATUS_UNLINK;
    ret->refcount = 1;
    return ret;
}

json_elem_item *new_null_item(void)
{
    return _new_item(N_NULL);
}

json_elem_item *new_bool_item(int val)
{
    json_elem_item *ret = _new_item(N_BOOLEAN);
    ret->value.boolval = val != 0;
    return ret;
}

json_elem_item *new_double_item(double val)
{
    json_elem_item *ret = _new_item(N_NUMBER);
    ret->value.numval = val;
    return ret;
}

json_elem_item *new_int_item(int64_t val)
{
    json_elem_item *ret = _new_item(N_INTEGER);
    ret->value.intval = val;
    return ret;
}

json_elem_item *new_string_item(const char *s, uint32_t len)
{
    json_elem_item *ret = _new_item(N_STRING);
    ret->value.strval.data = rmstrndup(s, len);
    ret->value.strval.len = len;
    return ret;
}

json_elem_item *new_cstring_item(const char *s)
{
    return new_string_item(s, strlen(s));
}

json_elem_item *new_keyval_item(const char *key, uint32_t len, json_elem_item *n)
{
    json_elem_item *ret = _new_item(N_KEYVAL);
    ret->value.kvval.key = rmstrndup(key, len);
    ret->value.kvval.val = n;
    return ret;
}

json_elem_item *new_array_item(uint32_t cap)
{
    json_elem_item *ret = _new_item(N_ARRAY);
    ret->value.arrval.cap = cap;
    ret->value.arrval.len = 0;
    ret->value.arrval.entries = (json_elem_item**)malloc(cap * sizeof(json_elem_item*));
    return ret;
}

json_elem_item *new_dict_item(uint32_t cap)
{
    json_elem_item *ret = _new_item(N_DICT);
    ret->value.dictval.cap = cap;
    ret->value.dictval.len = 0;
    ret->value.dictval.entries = (json_elem_item**)malloc(cap * sizeof(json_elem_item*));
    return ret;
}

static void item_array_make_room_for(json_elem_item *arr, uint32_t addlen)
{
    t_array *a = &arr->value.arrval;
    uint32_t newcap = a->len + addlen;

    if (a->cap >= newcap) return ;

    uint32_t nextcap = newcap;
    nextcap--;
    nextcap |= nextcap >> 1;
    nextcap |= nextcap >> 2;
    nextcap |= nextcap >> 4;
    nextcap |= nextcap >> 8;
    nextcap |= nextcap >> 16;
    nextcap++;

    const uint32_t CHUNK_SIZE = 1 << 20;
    if (nextcap > CHUNK_SIZE) {
        nextcap = ((newcap / CHUNK_SIZE) + 1) * CHUNK_SIZE;
    }

    a->cap = nextcap;
    a->entries = (json_elem_item**)realloc(a->entries, a->cap * sizeof(json_elem_item));
}

int item_array_append(json_elem_item *arr, json_elem_item *n)
{
    t_array *a = &arr->value.arrval;
    item_array_make_room_for(arr,1);
    a->entries[a->len++] = n;

    return OBJ_OK;
}

int item_array_set(json_elem_item *arr, int index, json_elem_item *n)
{
    t_array *a = &arr->value.arrval;

    if (index<0 || index >= a->len) {
        return OBJ_ERR;
    }
    a->entries[index] = n;

    return OBJ_OK;
}

int item_array_item(json_elem_item *arr, int index, json_elem_item **n)
{
    t_array *a = &arr->value.arrval;

    if (index < 0 || index >= a->len) {
        *n = NULL;
        return OBJ_ERR;
    }
    *n = a->entries[index];
    return OBJ_OK;
}

static json_elem_item *_obj_find(t_dict *o, const char *key, int *idx)
{
    for (int i = 0; i < o->len; i++) {
        if (!strcmp(key, o->entries[i]->value.kvval.key)) {
            if (idx) *idx = i;
            return o->entries[i];
        }
    }
    return NULL;
}

static void _obj_insert(t_dict *o, json_elem_item *n)
{
    if (o->len >= o->cap) {
        o->cap += o->cap ? (o->cap < 1024 * 1024 ? o->cap : 1024*1024) : 1;
        o->entries = (json_elem_item**)realloc(o->entries, o->cap * sizeof(t_keyval*));
    }
    o->entries[o->len++] = n;
}

int item_dict_set(json_elem_item *obj, const char *key, json_elem_item *n)
{
    t_dict *o = &obj->value.dictval;

    if (key == NULL) return OBJ_ERR;

    int idx;
    json_elem_item *kv = _obj_find(o, key, &idx);
    if (kv) {
        kv->value.kvval.val = n;
        return OBJ_OK;
    }
    kv = new_keyval_item(key, strlen(key),n);
    _obj_insert(o, kv);

    return OBJ_OK;
}

int item_dict_set_keyval(json_elem_item *obj, json_elem_item *kv, json_elem_item **old)
{
    t_dict *o = &obj->value.dictval;

    if (kv->value.kvval.key == NULL) return OBJ_ERR;

    int idx;
    json_elem_item *_kv = _obj_find(o, kv->value.kvval.key, &idx);
    if (_kv) {
        o->entries[idx] = kv;
        *old = _kv;
        return OBJ_OK;
    }
    _obj_insert(o, kv);
    return OBJ_OK;
}

int item_dict_get(json_elem_item *obj, const char *key, json_elem_item **val)
{
    if (key == NULL) return OBJ_ERR;

    t_dict *o = &obj->value.dictval;

    int idx = -1;
    json_elem_item *kv = _obj_find(o, key, &idx);

    if (!kv) return OBJ_ERR;

    *val = kv->value.kvval.val;
    return OBJ_OK;
}

/* Path */
static json_elem_item *path_item_eval(path_item *pn, json_elem_item *n, path_error *err) {
    *err = E_OK;
    if (!n) {
        goto badtype;
    }

    if (n->type == N_ARRAY) {
        json_elem_item *rn = NULL;
        int index = -1;

        // [수정] 타입이 NT_INDEX가 아니더라도, 키 값이 숫자라면 인덱스로 인정해줌
        if (NT_INDEX == pn->type) {
            index = pn->value.index;
        } else if (NT_KEY == pn->type) {
            // "0", "1" 같은 문자열을 숫자로 변환 시도
            char *endptr;
            index = (int)strtol(pn->value.key, &endptr, 10);
            if (*endptr != '\0') { // 숫자가 아닌 문자가 섞여있다면 실패
                goto badtype;
            }
        }

        if (index < 0) index = n->value.arrval.len + index;
        int rc = item_array_item(n, index, &rn);
        if (rc != OBJ_OK) {
            *err = E_NOINDEX;
        }
        return rn;
/*
        if (NT_INDEX == pn->type) {
            int index = pn->value.index;
            if (index < 0) index = n->value.arrval.len + index;
            int rc = item_array_item(n, index, &rn);
            if (rc != OBJ_OK) {
                *err = E_NOINDEX;
            }
        } else {
            goto badtype;
        }
        return rn;*/
    }
    if (n->type == N_DICT) {
        if (pn->type != NT_KEY) {
            goto badtype;
        }
        json_elem_item *rn = NULL;
        int rc = item_dict_get(n, pn->value.key, &rn);
        if (rc != OBJ_OK) {
            *err = E_NOKEY;
        }
        return rn;
    }
badtype:
    *err = E_BADTYPE;
    return NULL;
}

path_error search_path_find_ex(search_path *path, json_elem_item *root,
                               json_elem_item **n, json_elem_item **p)
{
    json_elem_item *current = root;
    json_elem_item *prev = NULL;
    path_error ret;

    for (int i = 0; i < path->len; i++) {
        if (path->nodes[i].type == NT_ROOT) {
            continue;
        }
        prev = current;
        current = path_item_eval(&path->nodes[i], current, &ret);
        if (ret != E_OK) {
            *p = prev;
            *n = NULL;
            return ret;
        }
    }
    *p = prev;
    *n = current;
    return E_OK;
}

search_path new_search_path(size_t cap)
{
    search_path sp;
    sp.len = 0;
    sp.cap = cap;
    sp.has_leading_dot = 0;
    return sp;
}

static void _search_path_append(search_path *p, path_item pn)
{
    if (p->len >= p->cap) {
        p->cap = p->cap ? (p->cap * 2 < 1024 ? p->cap * 2 : 1024) : 1;
    }
    p->nodes[p->len++] = pn;
}

static void search_path_append_index(search_path *p, int idx)
{
    path_item pn;
    pn.type = NT_INDEX;
    pn.value.index = idx;
    _search_path_append(p, pn);
}

static void search_path_append_key(search_path *p, const char *key, const size_t len)
{
    path_item pn;
    pn.type = NT_KEY;
    pn.value.key = rmstrndup(key, len);
    _search_path_append(p, pn);
}

static void search_path_append_root(search_path *p)
{
    path_item pn;
    pn.type = NT_ROOT;
    _search_path_append(p, pn);
}

static int _tokenize_path(const char *json, size_t len, search_path *path)
{
    tokenizer_state st = S_NULL;
    size_t offset = 0;
    char *pos = (char *)json;
    token tok;
    tok.len = 0;
    tok.s = pos;
    tok.type = T_KEY;

    while (offset <= len) {
        char c = (offset < len) ? *pos : '\0'; 
        
        switch (st) {
          case S_NULL:
               if (c == '$') {
                   tok.s = pos; tok.len = 1; tok.type = T_KEY;
                   st = S_IDENT;
               } else if (c == '.') {
                   st = S_ROOT;
               } else if (c == '[') {
                   st = S_BRACKET;
               } else if (isalpha(c) || c == '_') {
                   tok.s = pos; tok.len = 1; tok.type = T_KEY;
                   st = S_IDENT;
               } else if (c == '\0') {
                   // 빈 문자열 처리
               } else goto syntaxerror;
               break;

          case S_ROOT:
          case S_DOT:
               if (isalpha(c) || c == '$' || c == '_') {
                   tok.s = pos; tok.len = 1; st = S_IDENT;
               } else goto syntaxerror;
               break;

          case S_IDENT:
               if (c == '.' || c == '[' || c == '\0') {
                   if (tok.len == 1 && tok.s[0] == '$') search_path_append_root(path);
                   else if (tok.len > 0) search_path_append_key(path, tok.s, tok.len);
                   
                   if (c == '.') st = S_DOT;
                   else if (c == '[') st = S_BRACKET;
                   else st = S_NULL;
                   tok.len = 0; 
               } else if (isalnum(c) || c == '_' || c == '$') {
                   tok.len++;
               } else goto syntaxerror;
               break;

          case S_BRACKET:
               if (isdigit(c)) {
                   tok.s = pos; tok.len = 1; tok.type = T_INDEX; st = S_NUMBER;
               } else if (c == '-') {
                   tok.s = pos; tok.len = 1; tok.type = T_INDEX; st = S_MINUS;
               } else if (c == '"' || c == '\'') st = S_DKEY;
               else goto syntaxerror;
               break;

          case S_MINUS: // [추가] 마이너스 부호 처리
               if (isdigit(c)) {
                   tok.len++;
                   st = S_NUMBER;
               } else goto syntaxerror;
               break;

          case S_NUMBER:
               if (isdigit(c)) {
                   tok.len++;
               } else if (c == ']') {
                   int64_t num = 0;
                   int i = (tok.s[0] == '-') ? 1 : 0;
                   for (; i < (int)tok.len; i++) num = num * 10 + (tok.s[i] - '0');
                   if (tok.s[0] == '-') num = -num;
                   search_path_append_index(path, (int)num);
                   st = S_NULL;
                   tok.len = 0;
               } else goto syntaxerror;
               break;

          case S_DKEY:
          case S_SKEY:
               if (c == '"' || c == '\'') {
                   if (tok.len > 0) search_path_append_key(path, tok.s, tok.len);
                   st = S_NULL; 
               } else {
                   if (tok.len == 0) tok.s = pos;
                   tok.len++;
               }
               break;
        }

        if (offset < len) { offset++; pos++; }
        else break;
    }
    return (st == S_NULL || st == S_IDENT) ? PARSE_OK : PARSE_ERR;

syntaxerror:
    return PARSE_ERR;

    /*
    tokenizer_state st = S_NULL;
    size_t offset = 0;
    char *pos = (char *)json;
    token tok;
    tok.len = 0;
    tok.s = pos;

    while (offset < len) {
        char c = *pos;
        switch (st) {
          case S_NULL:
               switch (c) {
                 case '.':
                      tok.s++;
                      st = S_ROOT;
                      if (pos == json) {
                          path->has_leading_dot = 1;
                      }
                      break;
                 case '[':
                      tok.s++;
                      st = S_BRACKET;
                      break;
                 default:
                      if (isalpha(c) || '$' == c || '_' == c) {
                          tok.len++;
                          st = S_IDENT;
                          break;
                      }
                      goto syntaxerror;
               }
               break;
          case S_BRACKET:
               if (c == '"') {
                   tok.s++;
                   st = S_DKEY;
               } else if (c == '\'') {
                   tok.s++;
                   st = S_SKEY;
               } else if (isdigit(c)) {
                   tok.len++;
                   st = S_NUMBER;
               } else if ('-' == c) {
                   tok.len++;
                   st = S_MINUS;
               } else {
                   goto syntaxerror;
               }
               break;
          case S_ROOT:
          case S_DOT:
               if (isalpha(c) || '$' == c || '_' == c) {
                   tok.len++;
                   st = S_IDENT;
               } else {
                   goto syntaxerror;
               }
               break;
          case S_NUMBER:
               if (isdigit(c)) {
                   tok.len++;
                   break;
               }
               if (c == ']') {
                   st = S_NULL;
                   tok.type = T_INDEX;
                   pos++;
                   offset++;
                   goto tokenend;
               }
               goto syntaxerror;

          case S_IDENT:
               if (c == '.' || c == '[') {
                   //st = c == '.' ? S_DOT : S_BRACKET;
                   tok.type = T_KEY;
                   //pos ++;
                   //offset++;
                   goto tokenend;
               }
               if (!isalnum(c) && '$' != c && '_' != c) {
                   goto syntaxerror;
               }
               tok.len++;
               break;
          case S_DKEY:
               if (c == '"') {
                   if (offset < len - 1 && *(pos + 1) == ']') {
                       tok.type = T_KEY;
                       pos += 2;
                       offset += 2;
                       st = S_NULL;
                       goto tokenend;
                   } else {
                       goto syntaxerror;
                   }
               }
               tok.len++;
               break;
          case S_SKEY:
               if (c == '\'') {
                   if (offset < len - 1 && *(pos+1) == ']') {
                       tok.type = T_KEY;
                       pos += 2;
                       offset += 2;
                       st = S_NULL;
                       goto tokenend;
                   } else {
                       goto syntaxerror;
                   }
               }
               tok.len++;
               break;
          case S_MINUS:
               if (isdigit(c)) {
                   tok.len++;
                   st = S_NUMBER;
               } else {
                   goto syntaxerror;
               }
               break;
        }
        offset++;
        pos++;

        if (len == offset && (S_IDENT == st || S_ROOT == st)) {
            st = S_NULL;
            tok.type = T_KEY;
            goto tokenend;
        }
        continue;

        tokenend:
        {
            if (T_INDEX == tok.type) {
                int64_t num = 0;
                for (int i = !isdigit(tok.s[0]); i < tok.len; i++) {
                    int digit = tok.s[i]- '0';
                    num = num * 10 + digit;
                }
                if ('-' == tok.s[0]) num = -num;
                search_path_append_index(path, num);
            } else if (T_KEY == tok.type) {
                // 핵심 수정: 토큰 내용이 '$' 하나라면 무조건 루트로 판정
                if (tok.len == 1 && tok.s[0] == '$') {
                    search_path_append_root(path);
                } 
                // 토큰 내용이 '.' 하나일 때도 루트로 판정 (선택 사항)
                else if (tok.len == 1 && tok.s[0] == '.') {
                    search_path_append_root(path);
                } 
                else if (tok.len > 0) {
                    search_path_append_key(path, tok.s, tok.len);
                }
            }
            if (c == '.') st = S_DOT;
            else if (c == '[') st = S_BRACKET;
            else st = S_NULL;
            tok.s = pos;
            tok.len = 0;
        }
    }
    if (st == S_NULL || st == S_IDENT || st == S_ROOT) {
        return PARSE_OK;
    }
syntaxerror:
    return PARSE_ERR;*/
}

int parse_json_path(const char *json_path, size_t len,
                    search_path *path)
{
    return _tokenize_path(json_path, len, path);
}

int search_path_is_root_path(const search_path *sp)
{
    return (sp->len == 1 && sp->nodes[0].type == NT_ROOT);
}
