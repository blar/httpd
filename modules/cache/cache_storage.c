/* Copyright 2001-2006 The Apache Software Foundation or its licensors, as
 * applicable.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define CORE_PRIVATE

#include "mod_cache.h"

extern APR_OPTIONAL_FN_TYPE(ap_cache_generate_key) *cache_generate_key;

extern module AP_MODULE_DECLARE_DATA cache_module;

/* -------------------------------------------------------------- */

/*
 * delete all URL entities from the cache
 *
 */
int cache_remove_url(cache_request_rec *cache, apr_pool_t *p)
{
    cache_provider_list *list;
    cache_handle_t *h;

    list = cache->providers;

    /* Remove the stale cache entry if present. If not, we're
     * being called from outside of a request; remove the
     * non-stalle handle.
     */
    h = cache->stale_handle ? cache->stale_handle : cache->handle;
    if (!h) {
       return OK;
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, NULL,
                 "cache: Removing url %s from the cache", h->cache_obj->key);

    /* for each specified cache type, delete the URL */
    while(list) {
        list->provider->remove_url(h, p);
        list = list->next;
    }
    return OK;
}


/*
 * create a new URL entity in the cache
 *
 * It is possible to store more than once entity per URL. This
 * function will always create a new entity, regardless of whether
 * other entities already exist for the same URL.
 *
 * The size of the entity is provided so that a cache module can
 * decide whether or not it wants to cache this particular entity.
 * If the size is unknown, a size of -1 should be set.
 */
int cache_create_entity(request_rec *r, apr_off_t size)
{
    cache_provider_list *list;
    cache_handle_t *h = apr_pcalloc(r->pool, sizeof(cache_handle_t));
    char *key;
    apr_status_t rv;
    cache_request_rec *cache = (cache_request_rec *)
                         ap_get_module_config(r->request_config, &cache_module);

    rv = cache_generate_key(r, r->pool, &key);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    list = cache->providers;
    /* for each specified cache type, delete the URL */
    while (list) {
        switch (rv = list->provider->create_entity(h, r, key, size)) {
        case OK: {
            cache->handle = h;
            cache->provider = list->provider;
            cache->provider_name = list->provider_name;
            return OK;
        }
        case DECLINED: {
            list = list->next;
            continue;
        }
        default: {
            return rv;
        }
        }
    }
    return DECLINED;
}

static int set_cookie_doo_doo(void *v, const char *key, const char *val)
{
    apr_table_addn(v, key, val);
    return 1;
}

CACHE_DECLARE(void) ap_cache_accept_headers(cache_handle_t *h, request_rec *r,
                                            int preserve_orig)
{
    apr_table_t *cookie_table, *hdr_copy;
    const char *v;

    v = apr_table_get(h->resp_hdrs, "Content-Type");
    if (v) {
        ap_set_content_type(r, v);
        apr_table_unset(h->resp_hdrs, "Content-Type");
    }

    /* If the cache gave us a Last-Modified header, we can't just
     * pass it on blindly because of restrictions on future values.
     */
    v = apr_table_get(h->resp_hdrs, "Last-Modified");
    if (v) {
        ap_update_mtime(r, apr_date_parse_http(v));
        ap_set_last_modified(r);
        apr_table_unset(h->resp_hdrs, "Last-Modified");
    }

    /* The HTTP specification says that it is legal to merge duplicate
     * headers into one.  Some browsers that support Cookies don't like
     * merged headers and prefer that each Set-Cookie header is sent
     * separately.  Lets humour those browsers by not merging.
     * Oh what a pain it is.
     */
    cookie_table = apr_table_make(r->pool, 2);
    apr_table_do(set_cookie_doo_doo, cookie_table, r->err_headers_out,
                 "Set-Cookie", NULL);
    apr_table_do(set_cookie_doo_doo, cookie_table, h->resp_hdrs,
                 "Set-Cookie", NULL);
    apr_table_unset(r->err_headers_out, "Set-Cookie");
    apr_table_unset(h->resp_hdrs, "Set-Cookie");

    if (preserve_orig) {
        hdr_copy = apr_table_copy(r->pool, h->resp_hdrs);
        apr_table_overlap(hdr_copy, r->headers_out, APR_OVERLAP_TABLES_SET);
        r->headers_out = hdr_copy;
    }
    else {
        apr_table_overlap(r->headers_out, h->resp_hdrs, APR_OVERLAP_TABLES_SET);
    }
    if (!apr_is_empty_table(cookie_table)) {
        r->err_headers_out = apr_table_overlay(r->pool, r->err_headers_out,
                                               cookie_table);
    }
}

/*
 * select a specific URL entity in the cache
 *
 * It is possible to store more than one entity per URL. Content
 * negotiation is used to select an entity. Once an entity is
 * selected, details of it are stored in the per request
 * config to save time when serving the request later.
 *
 * This function returns OK if successful, DECLINED if no
 * cached entity fits the bill.
 */
int cache_select(request_rec *r)
{
    cache_provider_list *list;
    apr_status_t rv;
    cache_handle_t *h;
    char *key;
    cache_request_rec *cache = (cache_request_rec *)
                         ap_get_module_config(r->request_config, &cache_module);

    rv = cache_generate_key(r, r->pool, &key);
    if (rv != APR_SUCCESS) {
        return rv;
    }
    /* go through the cache types till we get a match */
    h = apr_palloc(r->pool, sizeof(cache_handle_t));

    list = cache->providers;

    while (list) {
        switch ((rv = list->provider->open_entity(h, r, key))) {
        case OK: {
            char *vary = NULL;
            int fresh;

            if (list->provider->recall_headers(h, r) != APR_SUCCESS) {
                /* TODO: Handle this error */
                return DECLINED;
            }

            /*
             * Check Content-Negotiation - Vary
             *
             * At this point we need to make sure that the object we found in
             * the cache is the same object that would be delivered to the
             * client, when the effects of content negotiation are taken into
             * effect.
             *
             * In plain english, we want to make sure that a language-negotiated
             * document in one language is not given to a client asking for a
             * language negotiated document in a different language by mistake.
             *
             * This code makes the assumption that the storage manager will
             * cache the req_hdrs if the response contains a Vary
             * header.
             *
             * RFC2616 13.6 and 14.44 describe the Vary mechanism.
             */
            vary = apr_pstrdup(r->pool, apr_table_get(h->resp_hdrs, "Vary"));
            while (vary && *vary) {
                char *name = vary;
                const char *h1, *h2;

                /* isolate header name */
                while (*vary && !apr_isspace(*vary) && (*vary != ','))
                    ++vary;
                while (*vary && (apr_isspace(*vary) || (*vary == ','))) {
                    *vary = '\0';
                    ++vary;
                }

                /*
                 * is this header in the request and the header in the cached
                 * request identical? If not, we give up and do a straight get
                 */
                h1 = apr_table_get(r->headers_in, name);
                h2 = apr_table_get(h->req_hdrs, name);
                if (h1 == h2) {
                    /* both headers NULL, so a match - do nothing */
                }
                else if (h1 && h2 && !strcmp(h1, h2)) {
                    /* both headers exist and are equal - do nothing */
                }
                else {
                    /* headers do not match, so Vary failed */
                    ap_log_error(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS,
                                r->server,
                                "cache_select_url(): Vary header mismatch.");
                    return DECLINED;
                }
            }

            cache->provider = list->provider;
            cache->provider_name = list->provider_name;

            /* Is our cached response fresh enough? */
            fresh = ap_cache_check_freshness(h, r);
            if (!fresh) {
                const char *etag, *lastmod;

                ap_log_error(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r->server,
                  "Cached response for %s isn't fresh.  Adding/replacing "
                  "conditional request headers.", r->uri);

                /* Make response into a conditional */
                cache->stale_headers = apr_table_copy(r->pool,
                                                      r->headers_in);

                /* We can only revalidate with our own conditionals: remove the
                 * conditions from the original request.
                 */
                apr_table_unset(r->headers_in, "If-Match");
                apr_table_unset(r->headers_in, "If-Modified-Since");
                apr_table_unset(r->headers_in, "If-None-Match");
                apr_table_unset(r->headers_in, "If-Range");
                apr_table_unset(r->headers_in, "If-Unmodified-Since");

                etag = apr_table_get(h->resp_hdrs, "ETag");
                lastmod = apr_table_get(h->resp_hdrs, "Last-Modified");

                if (etag || lastmod) {
                    /* If we have a cached etag and/or Last-Modified add in
                     * our own conditionals.
                     */

                    if (etag) {
                        apr_table_set(r->headers_in, "If-None-Match", etag);
                    }

                    if (lastmod) {
                        apr_table_set(r->headers_in, "If-Modified-Since",
                                      lastmod);
                    }
                    cache->stale_handle = h;
                }

                return DECLINED;
            }

            /* Okay, this response looks okay.  Merge in our stuff and go. */
            ap_cache_accept_headers(h, r, 0);

            cache->handle = h;
            return OK;
        }
        case DECLINED: {
            /* try again with next cache type */
            list = list->next;
            continue;
        }
        default: {
            /* oo-er! an error */
            return rv;
        }
        }
    }
    return DECLINED;
}

apr_status_t cache_generate_key_default(request_rec *r, apr_pool_t* p,
                                        char**key)
{
    char *port_str, *scheme, *hn;
    const char * hostname;
    int i;

    /*
     * Use the canonical name to improve cache hit rate, but only if this is
     * not a proxy request or if this is a reverse proxy request.
     * We need to handle both cases in the same manner as for the reverse proxy
     * case we have the following situation:
     *
     * If a cached entry is looked up by mod_cache's quick handler r->proxyreq
     * is still unset in the reverse proxy case as it only gets set in the
     * translate name hook (either by ProxyPass or mod_rewrite) which is run
     * after the quick handler hook. This is different to the forward proxy
     * case where it gets set before the quick handler is run (in the
     * post_read_request hook).
     * If a cache entry is created by the CACHE_SAVE filter we always have
     * r->proxyreq set correctly.
     * So we must ensure that in the reverse proxy case we use the same code
     * path and using the canonical name seems to be the right thing to do
     * in the reverse proxy case.
     */
    if (!r->proxyreq || (r->proxyreq == PROXYREQ_REVERSE)) {
        /* Use _default_ as the hostname if none present, as in mod_vhost */
        hostname =  ap_get_server_name(r);
        if (!hostname) {
            hostname = "_default_";
        }
    }
    else if(r->parsed_uri.hostname) {
        /* Copy the parsed uri hostname */
        hn = apr_pcalloc(p, strlen(r->parsed_uri.hostname) + 1);
        for (i = 0; r->parsed_uri.hostname[i]; i++) {
            hn[i] = apr_tolower(r->parsed_uri.hostname[i]);
        }
        /* const work-around */
        hostname = hn;
    }
    else {
        /* We are a proxied request, with no hostname. Unlikely
         * to get very far - but just in case */
        hostname = "_default_";
    }

    /* Copy the scheme, ensuring that it is lower case. If the parsed uri
     * contains no string or if this is not a proxy request.
     */
    if (r->proxyreq && r->parsed_uri.scheme) {
        /* Copy the scheme */
        scheme = apr_pcalloc(p, strlen(r->parsed_uri.scheme) + 1);
        for (i = 0; r->parsed_uri.scheme[i]; i++) {
            scheme[i] = apr_tolower(r->parsed_uri.scheme[i]);
        }
    }
    else {
        scheme = "http";
    }

    /* If the content is locally generated, use the port-number of the
     * current server. Otherwise. copy the URI's port-string (which may be a
     * service name). If the URI contains no port-string, use apr-util's
     * notion of the default port for that scheme - if available.
     */
    if(r->proxyreq) {
        if (r->parsed_uri.port_str) {
            port_str = apr_pcalloc(p, strlen(r->parsed_uri.port_str) + 2);
            port_str[0] = ':';
            for (i = 0; r->parsed_uri.port_str[i]; i++) {
                port_str[i + 1] = apr_tolower(r->parsed_uri.port_str[i]);
            }
        }
        else if (apr_uri_port_of_scheme(scheme)) {
            port_str = apr_psprintf(p, ":%u", apr_uri_port_of_scheme(scheme));
        }
        else {
            /* No port string given in the AbsoluteUri, and we have no
             * idea what the default port for the scheme is. Leave it
             * blank and live with the inefficiency of some extra cached
             * entities.
             */
            port_str = "";
        }
    }
    else {
        /* Use the server port */
        port_str = apr_psprintf(p, ":%u", ap_get_server_port(r));
    }

    /* Key format is a URI */
    *key = apr_pstrcat(p, scheme, "://", hostname, port_str,
                       r->parsed_uri.path, "?", r->args, NULL);

    return APR_SUCCESS;
}
