/**
 * Copyright (c) 2012 Anton Batenev
 * Copyright (c) 2012 Fernando Systems Ltd
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/**
 * Максимальная длина имени апстрима
 */
#ifndef NGX_HTTP_SLA_MAX_NAME_LEN
    #define NGX_HTTP_SLA_MAX_NAME_LEN 256
#endif

/**
 * Максимальное количество отслеживаемых статусов HTTP (минус 1 для суммарной статистики)
 */
#ifndef NGX_HTTP_SLA_MAX_HTTP_LEN
    #define NGX_HTTP_SLA_MAX_HTTP_LEN 32
#endif

/**
 * Максимальное количество отслеживаемых таймингов (минус 1 для "бесконечности")
 */
#ifndef NGX_HTTP_SLA_MAX_TIMINGS_LEN
    #define NGX_HTTP_SLA_MAX_TIMINGS_LEN 32
#endif

/**
 * Максимальное количество счетчиков в пуле (минус 1 для счетчика по умолчанию)
 */
#ifndef NGX_HTTP_SLA_MAX_COUNTERS_LEN
    #define NGX_HTTP_SLA_MAX_COUNTERS_LEN 16
#endif


/**
 * Данные счетчиков в shm
 */
typedef struct {
    u_char     name[NGX_HTTP_SLA_MAX_NAME_LEN];             /** Имя апстрима                            */
    ngx_uint_t name_len;                                    /** Длина имени апстрима                    */
    ngx_uint_t http[NGX_HTTP_SLA_MAX_HTTP_LEN];             /** Количество ответов HTTP                 */
    ngx_uint_t http_xxx[6];                                 /** Количество ответов в группах HTTP       */
    ngx_uint_t timings[NGX_HTTP_SLA_MAX_TIMINGS_LEN];       /** Количество ответов в интервале времени  */
    ngx_uint_t timings_agg[NGX_HTTP_SLA_MAX_TIMINGS_LEN];   /** Количество ответов до интервала времени */
} ngx_http_sla_pool_shm_t;

/**
 * Пул статистики
 */
typedef struct {
    ngx_str_t                name;       /** Имя пула               */
    ngx_array_t              timings;    /** Тайминги (ngx_uint_t)  */
    ngx_array_t              http;       /** Коды HTTP (ngx_uint_t) */
    ngx_slab_pool_t*         shm_pool;   /** Shared memory pool     */
    ngx_http_sla_pool_shm_t* shm_ctx;    /** Данные в shared memory */
} ngx_http_sla_pool_t;

/**
 * Основная конфигурация
 */
typedef struct {
    ngx_array_t          pools;          /** Пулы статистики  */
    ngx_http_sla_pool_t* default_pool;   /** Пул по умолчанию */
} ngx_http_sla_main_conf_t;

/**
 * Конфигурация location
 */
typedef struct {
    ngx_http_sla_pool_t* pool;   /** Пул для сбора статистики */
    ngx_uint_t           off;    /** Сбор статистики выключен */
} ngx_http_sla_loc_conf_t;


/* стандартные методы модуля nginx */
static ngx_int_t ngx_http_sla_init             (ngx_conf_t* cf);
static void*     ngx_http_sla_create_main_conf (ngx_conf_t* cf);
static void*     ngx_http_sla_create_loc_conf  (ngx_conf_t* cf);
static char*     ngx_http_sla_merge_loc_conf   (ngx_conf_t* cf, void* parent, void* child);

/**
 * Обработчик конфигурации sla_pool
 */
static char* ngx_http_sla_pool (ngx_conf_t* cf, ngx_command_t* cmd, void* conf);

/**
 * Обработчик конфигурации sla_pass
 */
static char* ngx_http_sla_pass (ngx_conf_t* cf, ngx_command_t* cmd, void* conf);

/**
 * Установка обработчика команды sla_stub
 */
static char* ngx_http_sla_status (ngx_conf_t* cf, ngx_command_t* cmd, void* conf);

/**
 * Установка обработчика команды sla_purge
 */
static char* ngx_http_sla_purge (ngx_conf_t* cf, ngx_command_t* cmd, void* conf);

/**
 * Обработчик вызова метода sla_stub - вывод статистических данных
 */
static ngx_int_t ngx_http_sla_status_handler (ngx_http_request_t* r);

/**
 * Обработчик вызова метода sla_purge - сброс статистических данных
 */
static ngx_int_t ngx_http_sla_purge_handler (ngx_http_request_t* r);

/**
 * Обработчик завершения запроса - сбор статистических данных
 */
static ngx_int_t ngx_http_sla_processor (ngx_http_request_t* r);

/**
 * Инициализация зоны shared memory
 */
static ngx_int_t ngx_http_sla_init_zone (ngx_shm_zone_t* shm_zone, void* data);

/**
 * Добавление значения таймингов или http кодов в список + проверка корректности значения
 */
static ngx_int_t ngx_http_sla_push_value (ngx_conf_t* cf, ngx_str_t* orig, ngx_int_t value, ngx_array_t* to, ngx_uint_t is_http);

/**
 * Парсинг списка таймингов или http кодов
 */
static ngx_int_t ngx_http_sla_parse_list (ngx_conf_t* cf, ngx_str_t* orig, ngx_uint_t offset, ngx_array_t* to, ngx_uint_t is_http);

/**
 * Поиск пула по имени
 */
static ngx_http_sla_pool_t* ngx_http_sla_get_pool (ngx_conf_t* cf, ngx_str_t* name);

/**
 * Поиск счетчика по имени или создание нового счетчика в пуле
 */
static ngx_http_sla_pool_shm_t* ngx_http_sla_get_counter (ngx_http_sla_pool_t* pool, ngx_str_t* name);

/**
 * Добавление счетчика в пул
 */
static ngx_http_sla_pool_shm_t* ngx_http_sla_add_counter (ngx_http_sla_pool_t* pool, ngx_str_t* name, ngx_uint_t hint);

/**
 * Установка HTTP кода в счетчике
 */
static ngx_int_t ngx_http_sla_set_http_status (ngx_http_sla_pool_t* pool, ngx_http_sla_pool_shm_t* counter, ngx_uint_t status);

/**
 * Установка времени обработки запроса в счетчике
 */
static ngx_int_t ngx_http_sla_set_http_time (ngx_http_sla_pool_t* pool, ngx_http_sla_pool_shm_t* counter, ngx_uint_t ms);

/**
 * Вывод статистики пула
 */
static ngx_int_t ngx_http_sla_print_pool (ngx_buf_t* buf, ngx_http_sla_pool_t* pool);

/**
 * Вывод статистики счетчика
 */
static ngx_int_t ngx_http_sla_print_counter (ngx_buf_t* buf, ngx_http_sla_pool_t* pool, ngx_http_sla_pool_shm_t* counter);


/**
 * Список команд
 */
static ngx_command_t ngx_http_sla_commands[] = {
    { ngx_string("sla_pool"),
      NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE1234,
      ngx_http_sla_pool,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("sla_pass"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_http_sla_pass,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("sla_status"),
      NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
      ngx_http_sla_status,
      0,
      0,
      NULL },

    { ngx_string("sla_purge"),
      NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
      ngx_http_sla_purge,
      0,
      0,
      NULL },

    ngx_null_command
};

/**
 * Методы инициализации модуля и конфигурации
 */
static ngx_http_module_t ngx_http_sla_module_ctx = {
    NULL,                            /* preconfiguration              */
    ngx_http_sla_init,               /* postconfiguration             */

    ngx_http_sla_create_main_conf,   /* create main configuration     */
    NULL,                            /* init main configuration       */

    NULL,                            /* create server configuration   */
    NULL,                            /* merge server configuration    */

    ngx_http_sla_create_loc_conf,    /* create location configuration */
    ngx_http_sla_merge_loc_conf      /* merge location configuration  */
};

/**
 * Описание модуля
 */
ngx_module_t ngx_http_sla_module = {
    NGX_MODULE_V1,
    &ngx_http_sla_module_ctx,   /* module context    */
    ngx_http_sla_commands,      /* module directives */
    NGX_HTTP_MODULE,            /* module type       */
    NULL,                       /* init master       */
    NULL,                       /* init module       */
    NULL,                       /* init process      */
    NULL,                       /* init thread       */
    NULL,                       /* exit thread       */
    NULL,                       /* exit process      */
    NULL,                       /* exit master       */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t ngx_http_sla_init (ngx_conf_t* cf)
{
    ngx_http_handler_pt*       handler;
    ngx_http_core_main_conf_t* config;

    config = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    handler = ngx_array_push(&config->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (handler == NULL) {
        return NGX_ERROR;
    }

    *handler = ngx_http_sla_processor;

    return NGX_OK;
}

static void* ngx_http_sla_create_main_conf (ngx_conf_t* cf)
{
    ngx_http_sla_main_conf_t* config;

    config = ngx_pcalloc(cf->pool, sizeof(ngx_http_sla_main_conf_t));
    if (config == NULL) {
        return NULL;
    }

    if (ngx_array_init(&config->pools, cf->pool, 4, sizeof(ngx_http_sla_pool_t)) != NGX_OK) {
        return NULL;
    }

    config->default_pool = NULL;

    return config;
}

static void* ngx_http_sla_create_loc_conf (ngx_conf_t* cf)
{
    ngx_http_sla_loc_conf_t* config;

    config = ngx_pcalloc(cf->pool, sizeof(ngx_http_sla_loc_conf_t));
    if (config == NULL) {
        return NULL;
    }

    return config;
}

static char* ngx_http_sla_merge_loc_conf (ngx_conf_t* cf, void* parent, void* child)
{
    ngx_http_sla_loc_conf_t*  prev    = parent;
    ngx_http_sla_loc_conf_t*  current = child;
    ngx_http_sla_main_conf_t* config;

    if (current->off != 0) {
        return NGX_CONF_OK;
    }

    config = ngx_http_conf_get_module_main_conf(cf, ngx_http_sla_module);

    if (current->pool != NULL) {
        return NGX_CONF_OK;
    }

    current->pool = prev->pool;

    if (current->pool == NULL) {
        current->pool = config->default_pool;
    }

    return NGX_CONF_OK;
}

static char* ngx_http_sla_pool (ngx_conf_t* cf, ngx_command_t* cmd, void* conf)
{
    ngx_uint_t                i;
    ngx_str_t*                value;
    ngx_uint_t*               pval;
    size_t                    size;
    ngx_shm_zone_t*           shm_zone;
    ngx_http_sla_pool_t*      pool;
    ngx_http_sla_main_conf_t* config = conf;

    value = cf->args->elts;

    /* проверка off пула */
    if (value[1].len == 3 && ngx_strncmp(value[1].data, "off", 3) == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invaid sla_pool name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    /* поиск среди имеющихся пулов */
    pool = config->pools.elts;
    for (i = 0; i < config->pools.nelts; i++) {
        if (pool[i].name.len == value[1].len && ngx_strcmp(pool[i].name.data, value[1].data) == 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "duplicate sla_pool name \"%V\"", &value[1]);
            return NGX_CONF_ERROR;
        }
    }

    /* инициализация нового пула */
    pool = ngx_array_push(&config->pools);
    if (pool == NULL) {
        return NGX_CONF_ERROR;
    }

    /* имя пула */
    pool->name.data = ngx_palloc(cf->pool, (value[1].len + 1) * sizeof(u_char));

    if (pool->name.data == NULL) {
        return NGX_CONF_ERROR;
    }

    pool->name.len = value[1].len;

    ngx_memcpy(pool->name.data, value[1].data, value[1].len);

    pool->name.data[value[1].len] = 0;

    /* массивы */
    if (ngx_array_init(&pool->timings, cf->pool, 4, sizeof(ngx_uint_t)) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (ngx_array_init(&pool->http, cf->pool, 16, sizeof(ngx_uint_t)) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    pool->shm_pool = NULL;
    pool->shm_ctx  = NULL;

    /* парсинг параметров */
    for (i = 2; i < cf->args->nelts; i++) {
        if (ngx_strncmp(value[i].data, "timings=", 8) == 0) {
            if (ngx_http_sla_parse_list(cf, &value[i], 8, &pool->timings, 0) != NGX_OK) {
                return NGX_CONF_ERROR;
            }
            continue;
        }

        if (ngx_strncmp(value[i].data, "http=", 5) == 0) {
            if (ngx_http_sla_parse_list(cf, &value[i], 5, &pool->http, 1) != NGX_OK) {
                return NGX_CONF_ERROR;
            }
            continue;
        }

        if (value[i].len == 7 && ngx_strncmp(value[i].data, "default", 7) == 0) {
            if (config->default_pool != NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "default sla_pool \"%V\" already defined", &config->default_pool->name);
                return NGX_CONF_ERROR;
            }
            config->default_pool = pool;
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid parameter \"%V\" for sla_pool", &value[i]);

        return NGX_CONF_ERROR;
    }

    /* заполнение параметрами по умолчанию */
    if (pool->timings.nelts == 0) {
        pval = ngx_array_push_n(&pool->timings, 3);

        pval[0] = 300;
        pval[1] = 500;
        pval[2] = 2000;
    }

    if (pool->http.nelts == 0) {
        pval = ngx_array_push_n(&pool->http, 13);

        pval[0]  = 200;   /* OK                    */
        pval[1]  = 301;   /* Moved Permanently     */
        pval[2]  = 302;   /* Moved Temporarily     */
        pval[3]  = 304;   /* Not Modified          */
        pval[4]  = 400;   /* Bad Request           */
        pval[5]  = 401;   /* Unauthorized          */
        pval[6]  = 403;   /* Forbidden             */
        pval[7]  = 404;   /* Not Found             */
        pval[8]  = 499;   /* Nginx special         */
        pval[9]  = 500;   /* Internal Server Error */
        pval[10] = 502;   /* Bad Gateway           */
        pval[11] = 503;   /* Service Unavailable   */
        pval[12] = 504;   /* Gateway Timeout       */
    }

    /* заполнение "хвостов" для учета общего числа */
    pval = ngx_array_push(&pool->timings);
    *pval = -1;
    pval = ngx_array_push(&pool->http);
    *pval = -1;

    /* проверка размеров */
    if (pool->http.nelts > NGX_HTTP_SLA_MAX_HTTP_LEN) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "http list too long for sla_pool");
        return NGX_CONF_ERROR;
    }

    if (pool->timings.nelts > NGX_HTTP_SLA_MAX_TIMINGS_LEN) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "timings list too long for sla_pool");
        return NGX_CONF_ERROR;
    }

    /* создание зоны shred memory */
    size = (sizeof(ngx_http_sla_pool_shm_t) * NGX_HTTP_SLA_MAX_COUNTERS_LEN / ngx_pagesize + 2) * ngx_pagesize;

    shm_zone = ngx_shared_memory_add(cf, &pool->name, size, &ngx_http_sla_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data != NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "sla_pool \"%V\" is already allocated", &pool->name);
        return NGX_CONF_ERROR;
    }

    shm_zone->data = pool;
    shm_zone->init = ngx_http_sla_init_zone;

    return NGX_CONF_OK;
}

static char* ngx_http_sla_pass (ngx_conf_t* cf, ngx_command_t* cmd, void* conf)
{
    ngx_str_t*               value;
    ngx_http_sla_pool_t*     pool;
    ngx_http_sla_loc_conf_t* config = conf;

    value = cf->args->elts;

    /* пул отключен */
    if (value[1].len == 3 && ngx_strncmp(value[1].data, "off", 3) == 0) {
        config->pool = NULL;
        config->off  = 1;
        return NGX_CONF_OK;
    }

    /* поиск пула */
    pool = ngx_http_sla_get_pool(cf, &value[1]);

    if (pool == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "sla_pool \"%V\" not found", &value[1]);
        return NGX_CONF_ERROR;
    }

    config->pool = pool;

    return NGX_CONF_OK;
}

static char* ngx_http_sla_status (ngx_conf_t* cf, ngx_command_t* cmd, void* conf)
{
    ngx_http_core_loc_conf_t* config;

    config = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    config->handler = ngx_http_sla_status_handler;

    return NGX_CONF_OK;
}

static char* ngx_http_sla_purge (ngx_conf_t* cf, ngx_command_t* cmd, void* conf)
{
    ngx_http_core_loc_conf_t* config;

    config = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    config->handler = ngx_http_sla_purge_handler;

    return NGX_CONF_OK;
}

static ngx_int_t ngx_http_sla_status_handler (ngx_http_request_t* r)
{
    ngx_uint_t                i;
    size_t                    size;
    ngx_buf_t*                buf;
    ngx_chain_t               out;
    ngx_int_t                 result;
    ngx_http_sla_pool_t*      pool;
    ngx_http_sla_main_conf_t* config;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "http sla handler");

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    result = ngx_http_discard_request_body(r);
    if (result != NGX_OK) {
        return result;
    }

    ngx_str_set(&r->headers_out.content_type, "text/plain");

    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;

        result = ngx_http_send_header(r);

        if (result == NGX_ERROR || result > NGX_OK || r->header_only) {
            return result;
        }
    }

    // TODO: 640K of memory should be enough for anybody :)
    size = 640 * 1024;

    buf = ngx_create_temp_buf(r->pool, size);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf  = buf;
    out.next = NULL;

    config = ngx_http_get_module_main_conf(r, ngx_http_sla_module);

    /* формирование результата */
    pool = config->pools.elts;

    for (i = 0; i < config->pools.nelts; i++) {
        ngx_shmtx_lock(&pool->shm_pool->mutex);

        result = ngx_http_sla_print_pool(buf, pool);

        ngx_shmtx_unlock(&pool->shm_pool->mutex);

        if (result != NGX_OK) {
            return result;
        }

        pool++;
    }

    /* отправка результата */
    r->headers_out.status           = NGX_HTTP_OK;
    r->headers_out.content_length_n = buf->last - buf->pos;

    buf->last_buf = (r == r->main) ? 1 : 0;

    result = ngx_http_send_header(r);
    if (result == NGX_ERROR || result > NGX_OK || r->header_only) {
        return result;
    }

    return ngx_http_output_filter(r, &out);
}

static ngx_int_t ngx_http_sla_purge_handler (ngx_http_request_t* r)
{
    ngx_uint_t                i;
    size_t                    size;
    ngx_buf_t*                buf;
    ngx_chain_t               out;
    ngx_int_t                 result;
    ngx_str_t                 name;
    ngx_http_sla_pool_t*      pool;
    ngx_http_sla_main_conf_t* config;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "http sla_purge handler");

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    result = ngx_http_discard_request_body(r);
    if (result != NGX_OK) {
        return result;
    }

    ngx_str_set(&r->headers_out.content_type, "text/plain");

    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;

        result = ngx_http_send_header(r);

        if (result == NGX_ERROR || result > NGX_OK || r->header_only) {
            return result;
        }
    }

    /* OK */
    size = 4 * sizeof(u_char);

    buf = ngx_create_temp_buf(r->pool, size);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf  = buf;
    out.next = NULL;

    buf->last = ngx_sprintf(buf->last, "OK\n");

    config = ngx_http_get_module_main_conf(r, ngx_http_sla_module);

    /* сброс данных */
    ngx_str_set(&name, "all");

    pool = config->pools.elts;

    for (i = 0; i < config->pools.nelts; i++) {
        ngx_shmtx_lock(&pool->shm_pool->mutex);

        ngx_memzero(pool->shm_ctx, sizeof(ngx_http_sla_pool_shm_t) * NGX_HTTP_SLA_MAX_COUNTERS_LEN);
        ngx_http_sla_add_counter(pool, &name, 0);

        ngx_shmtx_unlock(&pool->shm_pool->mutex);

        pool++;
    }

    /* отправка результата */
    r->headers_out.status           = NGX_HTTP_OK;
    r->headers_out.content_length_n = buf->last - buf->pos;

    buf->last_buf = (r == r->main) ? 1 : 0;

    result = ngx_http_send_header(r);
    if (result == NGX_ERROR || result > NGX_OK || r->header_only) {
        return result;
    }

    return ngx_http_output_filter(r, &out);
}

static ngx_int_t ngx_http_sla_processor (ngx_http_request_t* r)
{
    ngx_uint_t                 i;
    ngx_msec_int_t             ms;
    ngx_msec_int_t             time;
    ngx_uint_t                 status;
    ngx_http_sla_pool_shm_t*   counter;
    ngx_http_sla_loc_conf_t*   config;
    ngx_http_upstream_state_t* state;

    config = ngx_http_get_module_loc_conf(r, ngx_http_sla_module);

    if (config->off) {
        return NGX_OK;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "http sla proccessor");

    /* суммарное время ответов апстримов */
    time = 0;

    ngx_shmtx_lock(&config->pool->shm_pool->mutex);

    if (r->upstream_states != NULL && r->upstream_states->nelts > 0) {
        state = r->upstream_states->elts;

        for (i = 0; i < r->upstream_states->nelts; i++) {
            if (state[i].peer == NULL || state[i].status < 100 || state[i].status > 599) {
                continue;
            }

            ms = (ngx_msec_int_t)(state[i].response_sec * 1000 + state[i].response_msec);
            ms = ngx_max(ms, 0);

            time += ms;

            counter = ngx_http_sla_get_counter(config->pool, state[i].peer);
            if (counter == NULL) {
                ngx_shmtx_unlock(&config->pool->shm_pool->mutex);
                return NGX_ERROR;
            }

            ngx_http_sla_set_http_time(config->pool, counter, ms);
            ngx_http_sla_set_http_status(config->pool, counter, state[i].status);
        }
    }

    /* пул и счетчик по умолчанию */
    if (r->err_status) {
        status = r->err_status;
    } else if (r->headers_out.status) {
        status = r->headers_out.status;
    } else {
        status = 0;
    }

    ngx_http_sla_set_http_time(config->pool, config->pool->shm_ctx, time);
    ngx_http_sla_set_http_status(config->pool, config->pool->shm_ctx, status);

    ngx_shmtx_unlock(&config->pool->shm_pool->mutex);

    return NGX_OK;
}

static ngx_int_t ngx_http_sla_init_zone (ngx_shm_zone_t* shm_zone, void* data)
{
    ngx_str_t            name;
    ngx_http_sla_pool_t* pool;

    if (data != NULL) {
        /* TODO: проверить как такое решение работает */
        shm_zone->data = data;
        ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0, "sla uses old pools");
        return NGX_OK;
    }

    pool = shm_zone->data;

    pool->shm_pool = (ngx_slab_pool_t*)shm_zone->shm.addr;

    pool->shm_ctx = ngx_slab_alloc(pool->shm_pool, sizeof(ngx_http_sla_pool_shm_t) * NGX_HTTP_SLA_MAX_COUNTERS_LEN);
    if (pool->shm_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(pool->shm_ctx, sizeof(ngx_http_sla_pool_shm_t) * NGX_HTTP_SLA_MAX_COUNTERS_LEN);

    ngx_str_set(&name, "all");
    if (ngx_http_sla_add_counter(pool, &name, 0) == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t ngx_http_sla_push_value (ngx_conf_t* cf, ngx_str_t* orig, ngx_int_t value, ngx_array_t* to, ngx_uint_t is_http)
{
    ngx_uint_t* p;

    if (value == NGX_ERROR || value < 1 || value > 300000 /* 5 min */) {
        if (is_http == 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "incorrect timings values \"%V\" in sla_pool", orig);
        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "incorrect http values \"%V\" in sla_pool", orig);
        }
        return NGX_ERROR;
    }

    if (is_http != 0 && (value < 100 || value > 599)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "incorrect http values \"%V\" in sla_pool", orig);
        return NGX_ERROR;
    }

    p = ngx_array_push(to);

    if (p == NULL) {
        return NGX_ERROR;
    }

    *p = value;

    return NGX_OK;
}

static ngx_int_t ngx_http_sla_parse_list (ngx_conf_t* cf, ngx_str_t* orig, ngx_uint_t offset, ngx_array_t* to, ngx_uint_t is_http)
{
    u_char*   p1;
    u_char*   p2;
    ngx_int_t part;

    p1 = orig->data + offset;
    p2 = p1;

    while (p2 < orig->data + orig->len) {
        if (*p2 == ':') {
            part = ngx_atoi(p1, p2 - p1);

            if (ngx_http_sla_push_value(cf, orig, part, to, is_http) != NGX_OK) {
                return NGX_ERROR;
            }

            p1 = p2 + 1;
        }

        p2++;
    }

    if (p1 != p2) {
        part = ngx_atoi(p1, p2 - p1);
        if (ngx_http_sla_push_value(cf, orig, part, to, is_http) != NGX_OK) {
            return NGX_ERROR;
        }
    } else {
        ngx_http_sla_push_value(cf, orig, -1, to, is_http);
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_http_sla_pool_t* ngx_http_sla_get_pool (ngx_conf_t* cf, ngx_str_t* name)
{
    ngx_uint_t                i;
    ngx_http_sla_pool_t*      pool;
    ngx_http_sla_main_conf_t* config;

    config = ngx_http_conf_get_module_main_conf(cf, ngx_http_sla_module);

    pool = config->pools.elts;
    for (i = 0; i < config->pools.nelts; i++) {
        if (pool->name.len == name->len && ngx_strcmp(pool->name.data, name->data) == 0) {
            return pool;
        }
    }

    return NULL;
}

static ngx_http_sla_pool_shm_t* ngx_http_sla_get_counter (ngx_http_sla_pool_t* pool, ngx_str_t* name)
{
    ngx_uint_t               i;
    ngx_http_sla_pool_shm_t* counter;

    counter = pool->shm_ctx;
    for (i = 0; i < NGX_HTTP_SLA_MAX_COUNTERS_LEN; i++) {
        if (counter->name_len == name->len && ngx_strcmp(counter->name, name->data) == 0) {
            return counter;
        } else if (counter->name_len == 0) {
            return ngx_http_sla_add_counter(pool, name, i);
        }
        counter++;
    }

    return NULL;
}

static ngx_http_sla_pool_shm_t* ngx_http_sla_add_counter (ngx_http_sla_pool_t* pool, ngx_str_t* name, ngx_uint_t hint)
{
    ngx_uint_t               i;
    ngx_http_sla_pool_shm_t* result;

    result = pool->shm_ctx;
    for (i = hint; i < NGX_HTTP_SLA_MAX_COUNTERS_LEN; i++) {
        if (result->name_len == 0) {
            break;
        }
        result++;
    }

    if (i >= NGX_HTTP_SLA_MAX_COUNTERS_LEN) {
        return NULL;
    }

    if (name->len >= NGX_HTTP_SLA_MAX_NAME_LEN) {
        return NULL;
    }

    ngx_memcpy(result->name, name->data, name->len);
    result->name_len = name->len;

    return result;
}

static ngx_int_t ngx_http_sla_set_http_status (ngx_http_sla_pool_t* pool, ngx_http_sla_pool_shm_t* counter, ngx_uint_t status)
{
    ngx_uint_t  i;
    ngx_uint_t* http;

    if (status < 100 || status > 599) {
        return NGX_ERROR;
    }

    /* HTTP-xxx */
    counter->http_xxx[status / 100 - 1]++;
    counter->http_xxx[5]++;

    /* HTTP */
    http = pool->http.elts;
    for (i = 0; i < pool->http.nelts; i++) {
        if (*http == status) {
            counter->http[i]++;
            counter->http[pool->http.nelts - 1]++;
            break;
        }

        http++;
    }

    return NGX_OK;
}

static ngx_int_t ngx_http_sla_set_http_time (ngx_http_sla_pool_t* pool, ngx_http_sla_pool_shm_t* counter, ngx_uint_t ms)
{
    ngx_uint_t  i;
    ngx_uint_t* timing;

    /* нулевой тайминг не учитывается, т.к. это статика */
    if (ms == 0) {
        return NGX_OK;
    }

    timing = pool->timings.elts;

    for (i = 0; i < pool->timings.nelts; i++) {
        if (*timing > ms) {
            counter->timings[i]++;
            break;
        }

        timing++;
    }

    for ( ; i < pool->timings.nelts; i++) {
        counter->timings_agg[i]++;
    }

    return NGX_OK;
}

static ngx_int_t ngx_http_sla_print_pool (ngx_buf_t* buf, ngx_http_sla_pool_t* pool)
{
    ngx_uint_t i;
    ngx_int_t  result;

    for (i = 0; i < NGX_HTTP_SLA_MAX_COUNTERS_LEN; i++) {
        if (pool->shm_ctx[i].name_len == 0) {
            break;
        }

        result = ngx_http_sla_print_counter(buf, pool, &pool->shm_ctx[i]);
        if (result != NGX_OK) {
            return result;
        }
    }

    return NGX_OK;
}

static ngx_int_t ngx_http_sla_print_counter (ngx_buf_t* buf, ngx_http_sla_pool_t* pool, ngx_http_sla_pool_shm_t* counter)
{
    ngx_uint_t  i;
    ngx_uint_t* timing;
    ngx_uint_t* http_status;
    ngx_uint_t  http_count;
    ngx_uint_t  http_xxx_count;
    ngx_uint_t  timings_count;

    timing         = pool->timings.elts;
    timings_count  = counter->timings_agg[pool->timings.nelts - 1];
    http_status    = pool->http.elts;
    http_count     = counter->http[pool->http.nelts - 1];
    http_xxx_count = counter->http_xxx[5];

    /* коды http */
    buf->last = ngx_sprintf(buf->last, "%V.%s.http = %uA\n", &pool->name, counter->name, http_count);

    if (http_xxx_count > 0) {
        buf->last = ngx_sprintf(buf->last, "%V.%s.http.percent = %uA\n", &pool->name, counter->name, http_count * 100 / http_xxx_count);
    }

    for (i = 0; i < pool->http.nelts - 1; i++) {
        buf->last = ngx_sprintf(buf->last, "%V.%s.http_%uA = %uA\n", &pool->name, counter->name, http_status[i], counter->http[i]);

        if (http_count > 0) {
            buf->last = ngx_sprintf(buf->last, "%V.%s.http_%uA.percent = %uA\n", &pool->name, counter->name, http_status[i], counter->http[i] * 100 / http_count);
        }
    }

    /* группы кодов http */
    buf->last = ngx_sprintf(buf->last, "%V.%s.http_xxx = %uA\n", &pool->name, counter->name, http_xxx_count);
    buf->last = ngx_sprintf(buf->last, "%V.%s.http_xxx.percent = 100\n", &pool->name, counter->name);

    for (i = 0; i < 5; i++) {
        buf->last = ngx_sprintf(buf->last, "%V.%s.http_%uAxx = %uA\n", &pool->name, counter->name,  (i + 1) * 100, counter->http_xxx[i]);

        if (http_xxx_count > 0) {
            buf->last = ngx_sprintf(buf->last, "%V.%s.http_%uAxx.percent = %uA\n", &pool->name, counter->name, (i + 1) * 100, counter->http_xxx[i] * 100 / http_xxx_count);
        }
    }

    /* тайминги */
    for (i = 0; i < pool->timings.nelts; i++) {
        if (timing[i] != (ngx_uint_t)-1) {
            buf->last = ngx_sprintf(buf->last, "%V.%s.%uA = %uA\n", &pool->name, counter->name, timing[i], counter->timings[i]);

            if (timings_count > 0) {
                buf->last = ngx_sprintf(buf->last, "%V.%s.%uA.percent = %uA\n", &pool->name, counter->name, timing[i], counter->timings[i] * 100 / timings_count);
            }

            buf->last = ngx_sprintf(buf->last, "%V.%s.%uA.agg = %uA\n", &pool->name, counter->name, timing[i], counter->timings_agg[i]);

            if (timings_count > 0) {
                buf->last = ngx_sprintf(buf->last, "%V.%s.%uA.agg.percent = %uA\n", &pool->name, counter->name, timing[i], counter->timings_agg[i] * 100 / timings_count);
            }
        } else {
            buf->last = ngx_sprintf(buf->last, "%V.%s.inf = %uA\n", &pool->name, counter->name, counter->timings[i]);

            if (timings_count > 0) {
                buf->last = ngx_sprintf(buf->last, "%V.%s.inf.percent = %uA\n", &pool->name, counter->name, counter->timings[i] * 100 / timings_count);
            }

            buf->last = ngx_sprintf(buf->last, "%V.%s.inf.agg = %uA\n", &pool->name, counter->name, timings_count);
            buf->last = ngx_sprintf(buf->last, "%V.%s.inf.agg.percent = 100\n", &pool->name, counter->name);
        }
    }

    return NGX_OK;
}
