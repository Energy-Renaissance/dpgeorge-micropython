/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/stream.h"
#include "extmod/modnetwork.h"

#if MICROPY_PY_NETWORK_PPP_LWIP

#include "lwip/dns.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"

#define PPP_CLOSE_TIMEOUT_MS (4000)

typedef struct _network_ppp_obj_t {
    mp_obj_base_t base;
    bool active;
    bool connect_active;
    bool connected;
    volatile bool clean_close;
    int status;
    mp_obj_t stream;
    ppp_pcb *pcb;
    struct netif netif;
} network_ppp_obj_t;

const mp_obj_type_t mp_network_ppp_lwip_type;

static void network_ppp_status_cb(ppp_pcb *pcb, int err_code, void *ctx) {
    struct netif *netif = ppp_netif(pcb);
    network_ppp_obj_t *self = ctx;

    switch (err_code) {
        case PPPERR_NONE: {
            #if LWIP_DNS
            const ip_addr_t *ns;
            #endif /* LWIP_DNS */
            printf("status_cb: Connected\n");
            #if PPP_IPV4_SUPPORT
            printf("   our_ipaddr  = %s\n", ipaddr_ntoa(&netif->ip_addr));
            printf("   his_ipaddr  = %s\n", ipaddr_ntoa(&netif->gw));
            printf("   netmask     = %s\n", ipaddr_ntoa(&netif->netmask));
            #if LWIP_DNS
            ns = dns_getserver(0);
            printf("   dns1        = %s\n", ipaddr_ntoa(ns));
            ns = dns_getserver(1);
            printf("   dns2        = %s\n", ipaddr_ntoa(ns));
            #endif /* LWIP_DNS */
            #endif /* PPP_IPV4_SUPPORT */
            #if PPP_IPV6_SUPPORT
            printf("   our6_ipaddr = %s\n", ip6addr_ntoa(netif_ip6_addr(netif, 0)));
            #endif /* PPP_IPV6_SUPPORT */
            self->status = 1;

            #if CONFIG_LWIP_IPV6
            self->connected = (netif->ip_addr.u_addr.ip4.addr != 0);
            #else
            self->connected = (netif->ip_addr.addr != 0);
            #endif // CONFIG_LWIP_IPV6

            break;
        }
        case PPPERR_PARAM: {
            printf("status_cb: Invalid parameter\n");
            break;
        }
        case PPPERR_OPEN: {
            printf("status_cb: Unable to open PPP session\n");
            break;
        }
        case PPPERR_DEVICE: {
            printf("status_cb: Invalid I/O device for PPP\n");
            break;
        }
        case PPPERR_ALLOC: {
            printf("status_cb: Unable to allocate resources\n");
            break;
        }
        case PPPERR_USER: {
            printf("status_cb: User interrupt\n");
            self->clean_close = true;
            break;
        }
        case PPPERR_CONNECT: {
            printf("status_cb: Connection lost\n");
            self->connected = false;
            break;
        }
        case PPPERR_AUTHFAIL: {
            printf("status_cb: Failed authentication challenge\n");
            break;
        }
        case PPPERR_PROTOCOL: {
            printf("status_cb: Failed to meet protocol\n");
            break;
        }
        case PPPERR_PEERDEAD: {
            printf("status_cb: Connection timeout\n");
            break;
        }
        case PPPERR_IDLETIMEOUT: {
            printf("status_cb: Idle Timeout\n");
            break;
        }
        case PPPERR_CONNECTTIME: {
            printf("status_cb: Max connect time reached\n");
            break;
        }
        case PPPERR_LOOPBACK: {
            printf("status_cb: Loopback detected\n");
            break;
        }
        default: {
            printf("status_cb: Unknown error code %d\n", err_code);
            break;
        }
    }

    if (err_code == PPPERR_NONE) {
        printf("no error\n");
        return;
    }

    /* ppp_close() was previously called, don't reconnect */
    if (err_code == PPPERR_USER) {
        /* ppp_free(); -- can be called here */
        printf("user error\n");
        return;
    }
    printf("try reconnect\n");

    /*
     * Try to reconnect in 5 seconds, if you need a modem chatscript you have
     * to do a much better signaling here ;-)
     */
    self->status = -1;
    // ppp_connect(pcb, 5);
}

static mp_obj_t network_ppp_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    mp_obj_t stream = all_args[0];

    mp_get_stream_raise(stream, MP_STREAM_OP_READ | MP_STREAM_OP_WRITE);

    network_ppp_obj_t *self = mp_obj_malloc_with_finaliser(network_ppp_obj_t, type);
    self->stream = stream;
    self->active = false;
    self->connect_active = false;
    self->connected = false;
    self->clean_close = false;
    self->status = 0;

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t network_ppp___del__(mp_obj_t self_in) {
    network_ppp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->pcb != NULL) {
        ppp_free(self->pcb);
        self->pcb = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_ppp___del___obj, network_ppp___del__);

static void xxd(size_t len, const uint8_t *buf) {
    printf("(%d)", len);
    for (int i = 0; i < len; ++i) {
        printf(":%02x", buf[i]);
    }
    printf("=");
    for (int i = 0; i < len; ++i) {
        if (32 <= buf[i] && buf[i] <= 126) {
            printf("%c", buf[i]);
        } else {
            printf("<%02x>", buf[i]);
        }
    }
}

static mp_obj_t network_ppp_poll(mp_obj_t self_in) {
    network_ppp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint8_t buf[256];

    int err;
    mp_uint_t len = mp_stream_rw(self->stream, buf, sizeof(buf), &err, 0);
    if (len > 0) {
        printf("ppp_in(%u,", mp_hal_ticks_ms());
        xxd(len, buf);
        printf(")\n");
        pppos_input(self->pcb, (u8_t *)buf, len);
    }

    return MP_OBJ_NEW_SMALL_INT(len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_ppp_poll_obj, network_ppp_poll);

static mp_obj_t network_ppp_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    if (n_args != 1 && kwargs->used != 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("either pos or kw args are allowed"));
    }
    // network_ppp_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (kwargs->used != 0) {
        for (size_t i = 0; i < kwargs->alloc; i++) {
            if (mp_map_slot_is_filled(kwargs, i)) {
                switch (mp_obj_str_get_qstr(kwargs->table[i].key)) {
                    default:
                        break;
                }
            }
        }
        return mp_const_none;
    }

    if (n_args != 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("can query only one param"));
    }

    mp_obj_t val = mp_const_none;

    switch (mp_obj_str_get_qstr(args[1])) {
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
    }

    return val;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_ppp_config_obj, 1, network_ppp_config);

static mp_obj_t network_ppp_status(mp_obj_t self_in) {
    network_ppp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->status);
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_ppp_status_obj, network_ppp_status);

static u32_t network_ppp_output_callback(ppp_pcb *pcb, const void *data, u32_t len, void *ctx) {
    network_ppp_obj_t *self = ctx;
    int err;
    printf("ppp_out(%u,", mp_hal_ticks_ms());
    xxd(len, data);
    printf(")\n");
    return mp_stream_rw(self->stream, (void *)data, len, &err, MP_STREAM_RW_WRITE);
}

static mp_obj_t network_ppp_active(size_t n_args, const mp_obj_t *args) {
    network_ppp_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (n_args > 1) {
        if (mp_obj_is_true(args[1])) {
            if (self->active) {
                return mp_const_true;
            }

            self->pcb = pppos_create(&self->netif, network_ppp_output_callback, network_ppp_status_cb, self);

            if (self->pcb == NULL) {
                mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("init failed"));
            }

            self->active = true;
        } else {
            if (!self->active) {
                return mp_const_false;
            }

            if (self->connect_active) { // is connecting or connected?
                // Wait for PPPERR_USER, with timeout
                ppp_close(self->pcb, 0);
                // TODO make non-blocking, app can poll on ppp.status
                uint32_t t0 = mp_hal_ticks_ms();
                while (!self->clean_close && mp_hal_ticks_ms() - t0 < PPP_CLOSE_TIMEOUT_MS) {
                    network_ppp_poll(MP_OBJ_FROM_PTR(self));
                    mp_hal_delay_ms(10);
                }
            }

            // Release PPP
            ppp_free(self->pcb);
            self->pcb = NULL;
            self->active = false;
            self->connect_active = false;
            self->connected = false;
            self->clean_close = false;
        }
    }
    return mp_obj_new_bool(self->active);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_ppp_active_obj, 1, 2, network_ppp_active);

static mp_obj_t network_ppp_connect(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    enum { ARG_authmode, ARG_username, ARG_password };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_authmode, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = PPPAUTHTYPE_NONE} },
        { MP_QSTR_username, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_password, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };

    mp_arg_val_t parsed_args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed_args);

    network_ppp_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (!self->active) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("must be active"));
    }

    if (self->connect_active) {
        mp_raise_OSError(MP_EALREADY);
    }

    switch (parsed_args[ARG_authmode].u_int) {
        case PPPAUTHTYPE_NONE:
        case PPPAUTHTYPE_PAP:
        case PPPAUTHTYPE_CHAP:
            break;
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("invalid auth"));
    }

    if (parsed_args[ARG_authmode].u_int != PPPAUTHTYPE_NONE) {
        const char *username_str = mp_obj_str_get_str(parsed_args[ARG_username].u_obj);
        const char *password_str = mp_obj_str_get_str(parsed_args[ARG_password].u_obj);
        ppp_set_auth(self->pcb, parsed_args[ARG_authmode].u_int, username_str, password_str);
    }

    netif_set_default(self->pcb->netif);

    ppp_set_usepeerdns(self->pcb, true);

    if (ppp_connect(self->pcb, 0) != ERR_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("connect failed"));
    }

    self->connect_active = true;
    // TODO: create uart.irq

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_ppp_connect_obj, 1, network_ppp_connect);

static mp_obj_t network_ppp_isconnected(mp_obj_t self_in) {
    network_ppp_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->connected);
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_ppp_isconnected_obj, network_ppp_isconnected);

static mp_obj_t network_ppp_ifconfig(size_t n_args, const mp_obj_t *args) {
    network_ppp_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    return mod_network_nic_ifconfig(&self->netif, n_args - 1, args + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_ppp_ifconfig_obj, 1, 2, network_ppp_ifconfig);

static mp_obj_t network_ppp_ipconfig(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    network_ppp_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    return mod_network_nic_ipconfig(&self->netif, n_args - 1, args + 1, kwargs);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_ppp_ipconfig_obj, 1, network_ppp_ipconfig);

static const mp_rom_map_elem_t network_ppp_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&network_ppp___del___obj) },
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&network_ppp_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&network_ppp_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&network_ppp_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&network_ppp_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&network_ppp_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&network_ppp_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipconfig), MP_ROM_PTR(&network_ppp_ipconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&network_ppp_poll_obj) },

    { MP_ROM_QSTR(MP_QSTR_AUTH_NONE), MP_ROM_INT(PPPAUTHTYPE_NONE) },
    { MP_ROM_QSTR(MP_QSTR_AUTH_PAP), MP_ROM_INT(PPPAUTHTYPE_PAP) },
    { MP_ROM_QSTR(MP_QSTR_AUTH_CHAP), MP_ROM_INT(PPPAUTHTYPE_CHAP) },
};
static MP_DEFINE_CONST_DICT(network_ppp_locals_dict, network_ppp_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mp_network_ppp_lwip_type,
    MP_QSTR_PPP,
    MP_TYPE_FLAG_NONE,
    make_new, network_ppp_make_new,
    locals_dict, &network_ppp_locals_dict
    );

#endif
