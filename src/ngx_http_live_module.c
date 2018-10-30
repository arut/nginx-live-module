
/*
 * Copyright (C) Roman Arutyunyan
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_md5.h>


typedef struct {
    ngx_rbtree_t                rbtree;
    ngx_rbtree_node_t           sentinel;
} ngx_http_live_sh_t;


typedef struct {
    ngx_rbtree_t                rbtree;
    ngx_rbtree_node_t           sentinel;
    ngx_http_live_sh_t         *sh;
    ngx_slab_pool_t            *shpool;
    unsigned                    persistent:1;
    unsigned                    consistent:1;
    unsigned                    flush:1;
} ngx_http_live_t;


typedef struct {
    ngx_rbtree_node_t           node;
    u_char                      md5[16];
} ngx_http_live_node_t;


typedef struct {
    ngx_rbtree_node_t           node;
    u_char                     *md5;
    ngx_http_request_t         *request;
} ngx_http_live_get_node_t;


typedef struct {
    size_t                      size;
    ngx_http_live_t            *live;
    ngx_uint_t                  counter;
    ngx_uint_t                  last;  /* unsigned  last:1; */
    u_char                      md5[16];
    u_char                      data[1];
} ngx_http_live_msg_t;


typedef struct {
    ngx_str_t                   key;
    u_char                      md5[16];
    ngx_buf_t                  *buf;
    ngx_chain_t                *out;
    ngx_chain_t                *free;
    ngx_chain_t                *busy;
    ngx_http_live_t            *live;
    ngx_http_live_node_t       *node;
    ngx_http_live_get_node_t   *get;
    ngx_http_request_t         *request;
    ngx_uint_t                  counter;
    ngx_uint_t                  allocated;
    unsigned                    last_sent:1;
    unsigned                    done:1;
} ngx_http_live_ctx_t;


typedef struct {
    ngx_shm_zone_t             *shm_zone;
    ngx_uint_t                  methods;
    ngx_http_complex_value_t   *key;
    ngx_bufs_t                  bufs;
} ngx_http_live_loc_conf_t;


typedef struct {
    size_t                      buffer_size;
    ngx_socket_t               *write_fd;
    ngx_socket_t               *read_fd;
    ngx_uint_t                  nfd;
    ngx_buf_t                  *buf;
    ngx_connection_t          **conn;
    ngx_uint_t                  enabled;  /* unsigned  enabled:1; */
} ngx_http_live_main_conf_t;


static ngx_int_t ngx_http_live_handler(ngx_http_request_t *r);

static ngx_int_t ngx_http_live_get(ngx_http_request_t *r);
static void ngx_http_live_get_cleanup(void *data);
static ngx_http_live_get_node_t *ngx_http_live_get_rbtree_lookup(
    ngx_rbtree_t *rbtree, u_char *md5);
static void ngx_http_live_get_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static void ngx_http_live_get_write_handler(ngx_http_request_t *r);
static void ngx_http_live_get_data_handler(ngx_http_request_t *r, u_char *p,
    size_t size, ngx_uint_t counter, ngx_uint_t last);

static ngx_int_t ngx_http_live_post(ngx_http_request_t *r);
static void ngx_http_live_post_cleanup(void *data);
static ngx_http_live_node_t *ngx_http_live_post_rbtree_lookup(
    ngx_rbtree_t *rbtree, u_char *md5);
static void ngx_http_live_post_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static void ngx_http_live_post_read_handler(ngx_http_request_t *r);

static char *ngx_http_live_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_live_init_zone(ngx_shm_zone_t *shm_zone, void *data);
static char *ngx_http_live(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_http_live_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_live_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static void *ngx_http_live_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_live_init_main_conf(ngx_conf_t *cf, void *conf);

static ngx_int_t ngx_http_live_init_module(ngx_cycle_t *cycle);
static void ngx_http_live_notify_cleanup(void *data);
static ngx_int_t ngx_http_live_init_process(ngx_cycle_t *cycle);
static void ngx_http_live_notify_read_handler(ngx_event_t *rev);
static void ngx_http_live_notify_process_msg(ngx_http_live_msg_t *msg);
static void ngx_http_live_notify_write_handler(ngx_event_t *wev);
static ngx_int_t ngx_http_live_notify(ngx_http_request_t *r, ngx_buf_t *b);


static ngx_conf_bitmask_t  ngx_http_live_methods_mask[] = {
    { ngx_string("get"), NGX_HTTP_GET },
    { ngx_string("post"), NGX_HTTP_POST },
    { ngx_null_string, 0 }
};


static ngx_command_t  ngx_http_live_commands[] = {

    { ngx_string("live_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_ANY,
      ngx_http_live_zone,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("live_buffer_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_live_main_conf_t, buffer_size),
      NULL },

    { ngx_string("live"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_live,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("live_methods"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE12,
      ngx_conf_set_bitmask_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_live_loc_conf_t, methods),
      &ngx_http_live_methods_mask },

    { ngx_string("live_key"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_set_complex_value_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_live_loc_conf_t, key),
      NULL },

    { ngx_string("live_buffers"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_bufs_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_live_loc_conf_t, bufs),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_live_module_ctx = {
    NULL,                           /* preconfiguration */
    NULL,                           /* postconfiguration */

    ngx_http_live_create_main_conf, /* create main configuration */
    ngx_http_live_init_main_conf,   /* init main configuration */

    NULL,                           /* create server configuration */
    NULL,                           /* merge server configuration */

    ngx_http_live_create_loc_conf,  /* create location configuration */
    ngx_http_live_merge_loc_conf    /* merge location configuration */
};


ngx_module_t  ngx_http_live_module = {
    NGX_MODULE_V1,
    &ngx_http_live_module_ctx,      /* module context */
    ngx_http_live_commands,         /* module directives */
    NGX_HTTP_MODULE,                /* module type */
    NULL,                           /* init master */
    ngx_http_live_init_module,      /* init module */
    ngx_http_live_init_process,     /* init process */
    NULL,                           /* init thread */
    NULL,                           /* exit thread */
    NULL,                           /* exit process */
    NULL,                           /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_live_handler(ngx_http_request_t *r)
{
    ngx_md5_t                  md5;
    ngx_http_live_ctx_t       *ctx;
    ngx_http_live_loc_conf_t  *llcf;

    llcf = ngx_http_get_module_loc_conf(r, ngx_http_live_module);

    if (!(r->method & llcf->methods)) {
        return NGX_DECLINED;
    }

    if (llcf->shm_zone == NULL) {
        return NGX_DECLINED;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http live handler");

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_live_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_live_module);

    ctx->request = r;
    ctx->live = llcf->shm_zone->data;

    if (llcf->key == NULL) {
        ctx->key = r->uri;

    } else {
        if (ngx_http_complex_value(r, llcf->key, &ctx->key) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http live key: \"%V\"", &ctx->key);

    ngx_md5_init(&md5);
    ngx_md5_update(&md5, ctx->key.data, ctx->key.len);
    ngx_md5_final(ctx->md5, &md5);

#if (NGX_DEBUG)
    {
        u_char  hex[32];

        ngx_hex_dump(hex, ctx->md5, 16);

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http live md5: %*s", 32, hex);
    }
#endif

    switch (r->method) {
    case NGX_HTTP_GET:
        return ngx_http_live_get(r);

    default: /* NGX_HTTP_POST */
        return ngx_http_live_post(r);
    }
}


static ngx_int_t
ngx_http_live_get(ngx_http_request_t *r)
{
    ngx_int_t                  rc;
    ngx_http_live_t           *live;
    ngx_pool_cleanup_t        *cln;
    ngx_http_live_ctx_t       *ctx;
    ngx_http_live_node_t      *node;
    ngx_http_live_get_node_t  *get;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http live get handler");

    rc = ngx_http_discard_request_body(r);
    if (rc!= NGX_OK) {
        return rc;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_module);

    live = ctx->live;

    if (!live->persistent) {
        ngx_shmtx_lock(&live->shpool->mutex);
        node = ngx_http_live_post_rbtree_lookup(&live->sh->rbtree, ctx->md5);
        ngx_shmtx_unlock(&live->shpool->mutex);

        if (node == NULL) {
            ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                          "live stream not found");
            return NGX_HTTP_NOT_FOUND;
        }
    }

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cln->handler = ngx_http_live_get_cleanup;
    cln->data = ctx;

    get = ngx_pcalloc(r->pool, sizeof(ngx_http_live_get_node_t));
    if (get == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    get->md5 = ctx->md5;
    get->request = r;

    ngx_rbtree_insert(&live->rbtree, &get->node);

    ctx->get = get;

    r->headers_out.status = 200;
    r->headers_out.content_length_n = -1;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    r->write_event_handler = ngx_http_live_get_write_handler;

    r->main->count++;

    return NGX_DONE;
}


static void
ngx_http_live_get_cleanup(void *data)
{
    ngx_http_live_ctx_t *ctx = data;

    if (ctx->get) {
        ngx_rbtree_delete(&ctx->live->rbtree, &ctx->get->node);
    }
}


static ngx_http_live_get_node_t *
ngx_http_live_get_rbtree_lookup(ngx_rbtree_t *rbtree, u_char *md5)
{
    ngx_int_t                  rc;
    ngx_rbtree_node_t         *node, *sentinel;
    ngx_http_live_get_node_t  *get, *ret;

    /* this function returns the leftmost node having the given key */

    node = rbtree->root;
    sentinel = rbtree->sentinel;
    ret = NULL;

    while (node != sentinel) {
        get = (ngx_http_live_get_node_t *) node;

        rc = ngx_memcmp(md5, get->md5, 16);

        if (rc < 0) {
            node = node->left;
            continue;
        }

        if (rc > 0) {
            node = node->right;
            continue;
        }

        ret = get;
        node = node->left;
    }

    return ret;
}


static void
ngx_http_live_get_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t         **p;
    ngx_http_live_get_node_t   *get, *gett;

    for ( ;; ) {
        get = (ngx_http_live_get_node_t *) &node;
        gett = (ngx_http_live_get_node_t *) &temp;

        p = ngx_memcmp(get->md5, gett->md5, 16) < 0
            ? &temp->left : &temp->right;

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static void
ngx_http_live_get_write_handler(ngx_http_request_t *r)
{
    ngx_int_t                  rc;
    ngx_event_t               *wev;
    ngx_connection_t          *c;
    ngx_http_live_ctx_t       *ctx;
    ngx_http_core_loc_conf_t  *clcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http live get write handler");

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_module);

    c = r->connection;
    wev = c->write;

    clcf = ngx_http_get_module_loc_conf(r->main, ngx_http_core_module);

    if (wev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT, "client timed out");
        c->timedout = 1;

        ngx_http_finalize_request(r, NGX_HTTP_REQUEST_TIME_OUT);
        return;
    }

    if (wev->delayed) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "http live writer delayed");

        if (ngx_handle_write_event(wev, clcf->send_lowat) != NGX_OK) {
            ngx_http_finalize_request(r, NGX_ERROR);
        }

        return;
    }

    rc = ngx_http_output_filter(r, ctx->out);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http live output filter rc:%i", rc);

    if (rc == NGX_ERROR) {
        ngx_http_finalize_request(r, rc);
        return;
    }

    ngx_chain_update_chains(r->pool, &ctx->free, &ctx->busy,
                            &ctx->out, &ngx_http_live_module);

    if (ctx->done) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "http live close stream");
        ngx_http_finalize_request(r, ngx_http_send_special(r, NGX_HTTP_LAST));
        return;
    }

    if (!wev->delayed) {
        if (r->buffered || r->postponed || (r == r->main && c->buffered)) {
            ngx_add_timer(wev, clcf->send_timeout);

        } else if (wev->timer_set) {
            ngx_del_timer(wev);
        }
    }

    if (ngx_handle_write_event(wev, clcf->send_lowat) != NGX_OK) {
        ngx_http_finalize_request(r, NGX_ERROR);
    }
}


static void
ngx_http_live_get_data_handler(ngx_http_request_t *r, u_char *p, size_t size,
    ngx_uint_t counter, ngx_uint_t last)
{
    size_t                     n;
    ngx_buf_t                 *b;
    ngx_chain_t               *cl, **ll;
    ngx_http_live_t           *live;
    ngx_http_live_ctx_t       *ctx;
    ngx_http_live_loc_conf_t  *llcf;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http live get data n:%uz, l:%ui", size, last);

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_module);
    if (ctx == NULL) {
        return;
    }

    if (ctx->done) {
        return;
    }

    live = ctx->live;

    if (live->consistent && ctx->counter != counter) {
        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "live inconsistency: %ui/%ui", counter, ctx->counter);
        ctx->done = 1;
        return;
    }

    if (!live->persistent && last) {
        ctx->done = 1;
    }

    llcf = ngx_http_get_module_loc_conf(r, ngx_http_live_module);

    for (ll = &ctx->out; *ll; ll = &(*ll)->next);

    while (size) {
        if (ctx->free) {
            cl = ctx->free;
            ctx->free = cl->next;

        } else if (ctx->allocated < (ngx_uint_t) llcf->bufs.num) {
            cl = ngx_alloc_chain_link(r->pool);
            if (cl == NULL) {
                return;
            }

            cl->buf = ngx_create_temp_buf(r->pool, llcf->bufs.size);
            if (cl->buf == NULL) {
                return;
            }

            ctx->allocated++;

        } else {
            return;
        }

        *ll = cl;
        ll = &cl->next;
        cl->next = NULL;

        n = size > llcf->bufs.size ? llcf->bufs.size : size;

        b = cl->buf;
        b->tag = &ngx_http_live_module;
        b->flush = live->flush;
        b->pos = b->start;
        b->last = ngx_cpymem(b->pos, p, n);

        p += n;
        size -= n;
    }

    ctx->counter = last ? 0 : counter + 1;
}


static ngx_int_t
ngx_http_live_post(ngx_http_request_t *r)
{
    u_char                     *p;
    ngx_int_t                   rc;
    ngx_http_live_t            *live;
    ngx_pool_cleanup_t         *cln;
    ngx_http_live_ctx_t        *ctx;
    ngx_http_live_node_t       *node;
    ngx_http_live_main_conf_t  *lmcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http live post handler");

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_module);

    lmcf = ngx_http_get_module_main_conf(r, ngx_http_live_module);

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cln->handler = ngx_http_live_post_cleanup;
    cln->data = ctx;

    p = ngx_alloc(sizeof(ngx_buf_t) + lmcf->buffer_size, r->connection->log);
    if (p == NULL) {
        return NGX_ERROR;
    }

    ctx->buf = (ngx_buf_t *) p;
    ngx_memzero(ctx->buf, sizeof(ngx_buf_t));

    ctx->buf->tag = (ngx_buf_tag_t *) r;
    ctx->buf->start = p + sizeof(ngx_buf_t);
    ctx->buf->pos = ctx->buf->start;
    ctx->buf->last = ctx->buf->start;
    ctx->buf->end = ctx->buf->start + lmcf->buffer_size;

    live = ctx->live;

    ngx_shmtx_lock(&live->shpool->mutex);

    node = ngx_http_live_post_rbtree_lookup(&live->sh->rbtree, ctx->md5);
    if (node) {
        ngx_shmtx_unlock(&live->shpool->mutex);

        ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                      "live stream \"%V\" already exists", &ctx->key);
        return NGX_HTTP_CONFLICT;
    }

    node = ngx_slab_calloc_locked(live->shpool, sizeof(ngx_http_live_node_t));
    if (node == NULL) {
        ngx_shmtx_unlock(&live->shpool->mutex);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_memcpy(node->md5, ctx->md5, 16);

    ngx_rbtree_insert(&live->sh->rbtree, &node->node);

    ngx_shmtx_unlock(&live->shpool->mutex);

    ctx->node = node;

    r->request_body_no_buffering = 1;

    rc = ngx_http_read_client_request_body(r, ngx_http_live_post_read_handler);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}


static void
ngx_http_live_post_cleanup(void *data)
{
    ngx_http_live_ctx_t *ctx = data;

    size_t                n;
    u_char               *p;
    ngx_buf_t            *b;
    ngx_http_live_t      *live;
    ngx_http_live_msg_t  *msg;

    if (ctx->node) {
        live = ctx->live;

        if (!ctx->last_sent) {
            n = offsetof(ngx_http_live_msg_t, data);
            p = ngx_alloc(sizeof(ngx_buf_t) + n, ngx_cycle->log);

            if (p) {
                b = (ngx_buf_t *) p;
                ngx_memzero(b, sizeof(ngx_buf_t));
                b->pos = p + sizeof(ngx_buf_t);
                b->last = b->pos + n;

                msg = (ngx_http_live_msg_t *) b->pos;
                msg->size = n;
                msg->live = live;
                msg->counter = ctx->counter++;
                msg->last = 1;
                ngx_memcpy(msg->md5, ctx->md5, 16);

                if (ngx_http_live_notify(ctx->request, b) != NGX_OK) {
                    ngx_free(b);
                }
            }
        }

        ngx_shmtx_lock(&live->shpool->mutex);
        ngx_rbtree_delete(&live->sh->rbtree, &ctx->node->node);
        ngx_slab_free_locked(live->shpool, ctx->node);
        ngx_shmtx_unlock(&live->shpool->mutex);
    }

    if (ctx->buf) {
        if (ctx->buf->file_pos) {
            ctx->buf->tag = NULL;
        } else {
            ngx_free(ctx->buf);
        }

        ctx->buf = NULL;
    }
}


static ngx_http_live_node_t *
ngx_http_live_post_rbtree_lookup(ngx_rbtree_t *rbtree, u_char *md5)
{
    ngx_int_t              rc;
    ngx_rbtree_node_t     *node, *sentinel;
    ngx_http_live_node_t  *n;

    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {
        n = (ngx_http_live_node_t *) node;

        rc = ngx_memcmp(md5, n->md5, 16);

        if (rc < 0) {
            node = node->left;
            continue;
        }

        if (rc > 0) {
            node = node->right;
            continue;
        }

        return n;
    }

    return NULL;
}


static void
ngx_http_live_post_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t     **p;
    ngx_http_live_node_t   *n, *nt;

    for ( ;; ) {
        n = (ngx_http_live_node_t *) &node;
        nt = (ngx_http_live_node_t *) &temp;

        p = ngx_memcmp(n->md5, nt->md5, 16) < 0 ? &temp->left : &temp->right;

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static void
ngx_http_live_post_read_handler(ngx_http_request_t *r)
{
    size_t                    n, left;
    ngx_int_t                 rc;
    ngx_buf_t                *b;
    ngx_chain_t              *cl;
    ngx_event_t              *rev;
    ngx_http_live_msg_t      *msg;
    ngx_http_live_ctx_t      *ctx;
    ngx_http_request_body_t  *rb;

    if (ngx_exiting || ngx_terminate) {
        ngx_http_finalize_request(r, NGX_HTTP_CLOSE);
        return;
    }

    rev = r->connection->read;

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_module);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http live post read handler d:%d, r:%O",
                   rev->delayed, ctx->buf->file_pos);

    if (rev->delayed || ctx->buf->file_pos) {
        if (ngx_handle_read_event(rev, 0) != NGX_OK) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        }
        return;
    }

    rb = r->request_body;
    if (rb == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    b = ctx->buf;
    b->pos = b->start;
    b->last = b->pos + offsetof(ngx_http_live_msg_t, data);

    for ( ;; ) {
        if (rb->bufs == NULL && r->reading_body) {
            rc = ngx_http_read_unbuffered_request_body(r);

            if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
                ngx_http_finalize_request(r, rc);
                return;
            }
        }

        if (rb->bufs == NULL) {
            break;
        }

        cl = rb->bufs;
        n = cl->buf->last - cl->buf->pos;
        left = b->end - b->last;

        if (n > left) {
            n = left;
        }

        b->last = ngx_cpymem(b->last, cl->buf->pos, n);

        cl->buf->pos += n;

        if (b->last == b->end) {
            break;
        }

        rb->bufs = cl->next;
        ngx_free_chain(r->pool, cl);
    }

    if (b->last != b->pos + offsetof(ngx_http_live_msg_t, data)) {
        msg = (ngx_http_live_msg_t *) b->pos;
        msg->size = b->last - b->pos;
        msg->live = ctx->live;
        msg->counter = ctx->counter++;
        msg->last = (rb->bufs || r->reading_body) ? 0 : 1;
        ngx_memcpy(msg->md5, ctx->md5, 16);

        rc = ngx_http_live_notify(r, b);
        if (rc == NGX_ERROR) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }

        if (msg->last) {
            ctx->last_sent = 1;
        }
    }

    if (rb->bufs) {
        r->read_event_handler = ngx_http_live_post_read_handler;
        ngx_add_timer(rev, 1);
        rev->delayed = 1;
        return;
    }

    if (r->reading_body) {
        r->read_event_handler = ngx_http_live_post_read_handler;
        return;
    }

    ngx_http_finalize_request(r, NGX_HTTP_NO_CONTENT);
}


static char *
ngx_http_live_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_live_main_conf_t *lmcf = conf;

    size_t            size;
    u_char           *p;
    ngx_str_t         s, name, *value;
    ngx_uint_t        i;
    ngx_shm_zone_t   *shm_zone;
    ngx_http_live_t  *live;

    live = ngx_pcalloc(cf->pool, sizeof(ngx_http_live_t));
    if (live == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_rbtree_init(&live->rbtree, &live->sentinel,
                    ngx_http_live_get_rbtree_insert_value);

    name.len = 0;
    size = 0;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            name.data = value[i].data + 5;

            p = (u_char *) ngx_strchr(name.data, ':');

            if (p) {
                name.len = p - name.data;

                p++;

                s.len = value[i].data + value[i].len - p;
                s.data = p;

                size = ngx_parse_size(&s);
                if (size > 8191) {
                    continue;
                }
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid zone size \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }

        if (ngx_strcmp(value[i].data, "persistent") == 0) {
            live->persistent = 1;
            continue;
        }

        if (ngx_strcmp(value[i].data, "consistent") == 0) {
            live->consistent = 1;
            continue;
        }

        if (ngx_strcmp(value[i].data, "flush") == 0) {
            live->flush = 1;
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0 || size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "live_path must have \"zone\" parameter");
        return NGX_CONF_ERROR;
    }

    shm_zone = ngx_shared_memory_add(cf, &name, size, &ngx_http_live_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate zone \"%V\"", &name);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_live_init_zone;
    shm_zone->data = live;

    shm_zone->noreuse = 1;

    lmcf->enabled = 1;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_live_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    size_t            len;
    ngx_http_live_t  *live;

    live = shm_zone->data;

    live->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        live->sh = live->shpool->data;
        return NGX_OK;
    }

    live->sh = ngx_slab_alloc(live->shpool, sizeof(ngx_http_live_sh_t));
    if (live->sh == NULL) {
        return NGX_ERROR;
    }

    live->shpool->data = live->sh;

    ngx_rbtree_init(&live->sh->rbtree, &live->sh->sentinel,
                    ngx_http_live_post_rbtree_insert_value);

    len = sizeof(" in live zone \"\"") + shm_zone->shm.name.len;

    live->shpool->log_ctx = ngx_slab_alloc(live->shpool, len);
    if (live->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(live->shpool->log_ctx, " in live zone \"%V\"%Z",
                &shm_zone->shm.name);

    return NGX_OK;
}


static char *
ngx_http_live(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_live_loc_conf_t *llcf = conf;

    ngx_str_t                 *value;
    ngx_http_core_loc_conf_t  *clcf;

    value = cf->args->elts;

    llcf->shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                           &ngx_http_live_module);
    if (llcf->shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_live_handler;

    return NGX_CONF_OK;
}


static void *
ngx_http_live_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_live_loc_conf_t  *llcf;

    llcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_live_loc_conf_t));
    if (llcf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     llcf->shm_zone = NULL;
     *     llcf->methods = 0;
     *     llcf->key = NULL;
     */

    return llcf;
}


static char *
ngx_http_live_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_live_loc_conf_t *prev = parent;
    ngx_http_live_loc_conf_t *conf = child;

    ngx_conf_merge_bitmask_value(conf->methods, prev->methods,
                                 (NGX_CONF_BITMASK_SET|NGX_HTTP_GET));

    if (conf->key == NULL) {
        conf->key = prev->key;
    }

    ngx_conf_merge_bufs_value(conf->bufs, prev->bufs, 8, 8192);

    return NGX_CONF_OK;
}


static void *
ngx_http_live_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_live_main_conf_t  *lmcf;

    lmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_live_main_conf_t));
    if (lmcf == NULL) {
        return NULL;
    }

    lmcf->buffer_size = NGX_CONF_UNSET_SIZE;

    /*
     * set by ngx_pcalloc()
     *
     *     lmcf->write_fd = NULL;
     *     lmcf->read_fd = NULL;
     *     lmcf->nfd = 0;
     *     lmcf->buf = NULL;
     *     lmcf->enabled = 0;
     *     
     */

    return lmcf;
}


static char *
ngx_http_live_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_live_main_conf_t *lmcf = conf;

    ngx_conf_init_size_value(lmcf->buffer_size, 0);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_live_init_module(ngx_cycle_t *cycle)
{
    int                         value;
    size_t                      buffer_size;
    socklen_t                   olen;
    ngx_uint_t                  n, nfd;
    ngx_socket_t               *fd, fds[2];
    ngx_core_conf_t            *ccf;
    ngx_pool_cleanup_t         *cln;
    ngx_http_live_main_conf_t  *lmcf;

    lmcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_live_module);

    if (lmcf == NULL || !lmcf->enabled) {
        return NGX_OK;
    }

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);

    nfd = ccf->worker_processes;

    fd = ngx_palloc(cycle->pool, nfd * 2 * sizeof(ngx_socket_t));
    if (fd == NULL) {
        return NGX_ERROR;
    }

    lmcf->read_fd = fd;
    lmcf->write_fd = fd + nfd;

    cln = ngx_pool_cleanup_add(cycle->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }

    cln->handler = ngx_http_live_notify_cleanup;
    cln->data = lmcf;

    buffer_size = 0;

    for (n = 0; n < nfd; n++) {
        if (socketpair(AF_UNIX,
#if !defined(SOCK_SEQPACKET) || (NGX_DARWIN)
                       SOCK_DGRAM,
#else
                       SOCK_SEQPACKET,
#endif
                       0, fds))
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_socket_errno,
                          "socketpair() failed");
            return NGX_ERROR;
        }

        ngx_log_error(NGX_LOG_INFO, cycle->log, 0,
                       "http live socketpair w:%d, %d<-%d",
                       n, (int) fds[0], (int) fds[1]);

        lmcf->read_fd[n] = fds[0];
        lmcf->write_fd[n] = fds[1];

        lmcf->nfd++;

        if (ngx_nonblocking(fds[0]) == -1
            || ngx_nonblocking(fds[1]) == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_socket_errno,
                          ngx_nonblocking_n " failed");
            return NGX_ERROR;
        }

        if (lmcf->buffer_size) {
            value = lmcf->buffer_size;

            if (setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &value, sizeof(int))
                == -1)
            {
                ngx_log_error(NGX_LOG_CRIT, cycle->log, ngx_socket_errno,
                              "setsockopt(SO_SNDBUF) live notify failed");
                return NGX_ERROR;
            }
        }

        if (buffer_size == 0) {
            olen = sizeof(int);

            if (getsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &value, &olen) == -1) {
                ngx_log_error(NGX_LOG_CRIT, cycle->log, ngx_socket_errno,
                              "getsockopt(SO_SNDBUF) live notify failed");
                return NGX_ERROR;
            }

            if (value <= 64) {
                ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                              "too small live notify buffer");
                return NGX_ERROR;
            }

            /*
             * On BSD/Macos buffer_size is the full socket queue length.
             * It should not exceed (net.local.dgram.recvspace - 16).
             */

            buffer_size = value;

#if (NGX_LINUX)
            /*
             * On Linux buffer_size is max length of a single packet.
             * It is 32 bytes less than SO_SNDBUF returned by setsockopt().
             */

            buffer_size -= 32;
#endif

            if (buffer_size < sizeof(ngx_http_live_msg_t)) {
                ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                              "too small live notify buffer");
                return NGX_ERROR;
            }

            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                          "live notify buffer size:%uz", buffer_size);
        }
    }

    lmcf->buffer_size = buffer_size;

    lmcf->buf = ngx_create_temp_buf(cycle->pool, buffer_size);
    if (lmcf->buf == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_http_live_notify_cleanup(void *data)
{
    ngx_http_live_main_conf_t *lmcf = data;

    ngx_uint_t  n;

    if (lmcf->read_fd && lmcf->write_fd) {
        for (n = 0; n < lmcf->nfd; n++) {
            (void) ngx_close_socket(lmcf->read_fd[n]);
            (void) ngx_close_socket(lmcf->write_fd[n]);
        }
    }
}


static ngx_int_t
ngx_http_live_init_process(ngx_cycle_t *cycle)
{
    ngx_int_t                   event;
    ngx_uint_t                  n;
    ngx_event_t                *rev, *wev;
    ngx_socket_t                fd;
    ngx_connection_t           *c;
    ngx_http_live_main_conf_t  *lmcf;

    lmcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_live_module);

    if (lmcf == NULL || lmcf->nfd == 0) {
        return NGX_OK;
    }

    fd = NGX_INVALID_FILE;

    for (n = 0; n < lmcf->nfd; n++) {
        if (n == ngx_worker) {
            fd = lmcf->read_fd[n];
            continue;
        }

        if (ngx_close_socket(lmcf->read_fd[n]) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_socket_errno,
                          ngx_close_socket_n " live_notify_socket[%ui] failed",
                          n);
        }
    }

    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "worker has no live notify socket");
        return NGX_ERROR;
    }

    event = (ngx_event_flags & NGX_USE_CLEAR_EVENT) ? NGX_CLEAR_EVENT
                                                    : NGX_LEVEL_EVENT;

    c = ngx_get_connection(fd, cycle->log);
    if (c == NULL) {
        return NGX_ERROR;
    }

    c->recv = ngx_udp_recv;
    c->data = lmcf;

    rev = c->read;
    rev->channel = 1;
    rev->log = cycle->log;
    rev->handler = ngx_http_live_notify_read_handler;

    if (ngx_add_event(rev, NGX_READ_EVENT, event) != NGX_OK) {
        return NGX_ERROR;
    }

    lmcf->conn = ngx_pcalloc(ngx_cycle->pool,
                             lmcf->nfd * sizeof(ngx_connection_t *));
    if (lmcf->conn == NULL) {
        return NGX_ERROR;
    }

    for (n = 0; n < lmcf->nfd; n++) {
        c = ngx_get_connection(lmcf->write_fd[n], cycle->log);
        if (c == NULL) {
            return NGX_ERROR;
        }

        lmcf->conn[n] = c;

        c->send = ngx_send;
        c->data = NULL;

        rev = c->read;
        rev->channel = 1;

        wev = c->write;
        wev->log = cycle->log;
        wev->handler = ngx_http_live_notify_write_handler;

        if (ngx_add_event(wev, NGX_WRITE_EVENT, event) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    lmcf->read_fd = NULL;
    lmcf->write_fd = NULL;

    return NGX_OK;
}


static void
ngx_http_live_notify_read_handler(ngx_event_t *rev)
{
    ssize_t                     n;
    ngx_buf_t                  *b;
    ngx_connection_t           *c;
    ngx_http_live_msg_t        *msg;
    ngx_http_live_main_conf_t  *lmcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "http live notify read handler");

    c = rev->data;
    lmcf = c->data;
    b = lmcf->buf;

    while (rev->ready) {
        n = c->recv(c, b->start, b->end - b->start);
        if (n <= 0) {
            break;
        }

        if ((size_t) n < offsetof(ngx_http_live_msg_t, data)) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "live notify message format error");
            continue;
        }

        msg = (ngx_http_live_msg_t *) b->start;

        if ((size_t) n != msg->size) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "live notify message size error");
            continue;
        }

#if (NGX_DEBUG)
        {
            u_char  hex[32];

            ngx_hex_dump(hex, msg->md5, 16);

            ngx_log_debug5(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                           "http live notify recv n:%z, c:%ui, l:%d, md5:%*s",
                           n, msg->counter, msg->last, 32, hex);
        }
#endif

        ngx_http_live_notify_process_msg(msg);
    }

    (void) ngx_handle_read_event(rev, 0);
}


static void
ngx_http_live_notify_process_msg(ngx_http_live_msg_t *msg)
{
    u_char                    *p;
    size_t                     size;
    ngx_connection_t          *c;
    ngx_http_request_t        *r;
    ngx_http_live_get_node_t  *get;

    p = &msg->data[0];
    size = msg->size - offsetof(ngx_http_live_msg_t, data);

    get = ngx_http_live_get_rbtree_lookup(&msg->live->rbtree, msg->md5);

    while (get) {
        r = get->request;
        c = r->connection;

        ngx_http_live_get_data_handler(r, p, size, msg->counter, msg->last);

        ngx_post_event(c->write, &ngx_posted_events);

        if (r != c->data) {
            ngx_http_post_request(r, NULL);
        }

        get = (ngx_http_live_get_node_t *)
                  ngx_rbtree_next(&msg->live->rbtree, &get->node);

        if (get && ngx_memcmp(get->md5, msg->md5, 16) != 0) {
            break;
        }
    }
}


static void
ngx_http_live_notify_write_handler(ngx_event_t *wev)
{
    ssize_t              n;
    ngx_buf_t           *b;
    ngx_chain_t         *cl;
    ngx_connection_t    *c;
    ngx_http_request_t  *r;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "http live notify write handler");

    c = wev->data;

    while (wev->ready && c->data) {
        cl = c->data;
        b = cl->buf;

        n = c->send(c, b->pos, b->last - b->pos);

        if (n == NGX_AGAIN) {
            break;
        }

        if (n == NGX_ERROR) {
            /*
             * This can be a serious socket error, as well as
             * ENOBUFS on BSD systems when SOCK_DGRAM is used.
             * Anyway, there's not much we can do at this point
             * rather than retry the send after a few millisec.
             *
             * This situation is common on MacOS however.
             * Being a BSD system and missing a SOCK_SEQPACKET
             * implementation (despite having a #define), it may
             * return ENOBUFS here.  Typically for BSD, there is
             * no way to wait for ENOBUFS condition to end with
             * kqueue/select.  The only available solution is
             * waiting on a timer.
             */

            wev->cancelable = 1;
            wev->error = 0;

            ngx_add_timer(wev, 10);

            break;
        }

        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "http live notify send b:%p, n:%z, r:%O",
                       b, n, b->file_pos);

        c->data = cl->next;

        ngx_free_chain(ngx_cycle->pool, cl);

        if (--b->file_pos) {
            continue;
        }

        if (b->tag == NULL) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                           "http live notify free buffer b:%p", b);
            ngx_free(b);
            continue;
        }

        r = (ngx_http_request_t *) b->tag;

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http live notify reclaim buffer b:%p", b);

        ngx_post_event(r->connection->read, &ngx_posted_events);

        if (r != r->connection->data) {
            ngx_http_post_request(r, NULL);
        }
    }

    (void) ngx_handle_write_event(wev, 0);
}


static ngx_int_t
ngx_http_live_notify(ngx_http_request_t *r, ngx_buf_t *b)
{
    ngx_uint_t                  n;
    ngx_chain_t                *cl, **ll;
    ngx_connection_t           *c;
    ngx_http_live_main_conf_t  *lmcf;

#if (NGX_DEBUG)
    {
        u_char                hex[32];
        ngx_http_live_msg_t  *msg;

        msg = (ngx_http_live_msg_t *) b->pos;
        ngx_hex_dump(hex, msg->md5, 16);

        ngx_log_debug7(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                      "http live notify b:%p, n:%z, c:%ui, l:%d, t:%p, md5:%*s",
                      b, msg->size, msg->counter, msg->last, b->tag, 32, hex);
    }
#endif

    lmcf = ngx_http_get_module_main_conf(r, ngx_http_live_module);

    for (n = 0; n < lmcf->nfd; n++) {
        cl = ngx_alloc_chain_link(ngx_cycle->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        b->file_pos++;

        cl->buf = b;
        cl->next = NULL;

        c = lmcf->conn[n];

        for (ll = (ngx_chain_t **) &c->data; *ll; ll = &(*ll)->next);
        *ll = cl;

        if (c->data == cl && c->write->ready) {
            ngx_post_event(c->write, &ngx_posted_events);
        }
    }

    if (b->file_pos == 0 && b->tag == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "http live notify free buffer b:%p", b);
        ngx_free(b);
    }

    return NGX_OK;
}
