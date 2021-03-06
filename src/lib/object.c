/* SPDX-License-Identifier: BSD-2-Clause */

#include <limits.h>
#include <stdlib.h>

#include "attrs.h"
#include "checks.h"
#include "db.h"
#include "emitter.h"
#include "log.h"
#include "object.h"
#include "pkcs11.h"
#include "session_ctx.h"
#include "token.h"
#include "utils.h"

typedef struct tobject_match_list tobject_match_list;
struct tobject_match_list {
    CK_OBJECT_HANDLE tobj_handle;
    CK_OBJECT_CLASS class;
    tobject_match_list *next;
};

typedef struct object_find_data object_find_data;
struct object_find_data {
    tobject_match_list *head;
    tobject_match_list *cur;
};

void tobject_free(tobject *tobj) {

    if (!tobj) {
        return;
    }

    twist_free(tobj->objauth);
    twist_free(tobj->priv);
    twist_free(tobj->pub);

    twist_free(tobj->unsealed_auth);

    attr_list *a = tobject_get_attrs(tobj);
    attr_list_free(a);
    free(tobj);
}

void sealobject_free(sealobject *sealobj) {
    twist_free(sealobj->soauthsalt);
    twist_free(sealobj->sopriv);
    twist_free(sealobj->sopub);
    twist_free(sealobj->userauthsalt);
    twist_free(sealobj->userpub);
    twist_free(sealobj->userpriv);
}

CK_RV object_mech_is_supported(tobject *tobj, CK_MECHANISM_PTR mech) {

    CK_ATTRIBUTE_PTR a = attr_get_attribute_by_type(tobj->attrs, CKA_ALLOWED_MECHANISMS);
    if (!a) {
        LOGE("Expected object to have: CKA_ALLOWED_MECHANISMS");
        return CKR_GENERAL_ERROR;
    }

    CK_ULONG count = a->ulValueLen/sizeof(CK_MECHANISM_TYPE);
    CK_MECHANISM_TYPE_PTR mt = (CK_MECHANISM_TYPE_PTR)a->pValue;

    CK_ULONG i;
    for(i=0; i < count; i++) {
        CK_MECHANISM_TYPE t = mt[i];
        if (t == mech->mechanism) {
            return CKR_OK;
        }
    }

    /* TODO further sanity checking for CKR_MECHANISM_PARAM_INVALID */

    return CKR_MECHANISM_INVALID;
}

static bool attr_filter(attr_list *attrs, CK_ATTRIBUTE_PTR templ, CK_ULONG count) {


    CK_ULONG i;
    // If ulCount is set to 0 all items match automatically.
    if (count == 0) {
	    return true;
    }

    for (i=0; i < count; i++) {
        CK_ATTRIBUTE_PTR search = &templ[i];

        CK_ATTRIBUTE_PTR compare = NULL;
        bool is_attr_match = false;
        CK_ULONG j;
        for(j=0; j < attr_list_get_count(attrs); j++) {
            const CK_ATTRIBUTE_PTR ptr = attr_list_get_ptr(attrs);
            compare = &ptr[j];

            if (search->type != compare->type) {
                continue;
            }

            if (search->ulValueLen != compare->ulValueLen) {
                continue;
            }

            bool match = !memcmp(compare->pValue, search->pValue, search->ulValueLen);
            if (match) {
                is_attr_match = true;
                break;
            }
        }

        /*
         * If we didn't get an attribute match, then the searched for attribute wasn't
         * found and it's not a match. Ie search attribute set must be subset of compare
         * attribute set
         */

        if (!is_attr_match) {
            return false;
        }

    }

    /*
     * Done with matching loops, we always found a match, thus we have a match
     * assign it.
     */

    /* all the specified template attributes matched */
    return true;
}

tobject *object_attr_filter(tobject *tobj, CK_ATTRIBUTE_PTR templ, CK_ULONG count) {

    attr_list *attrs = tobject_get_attrs(tobj);
    bool res = attr_filter(attrs, templ, count);
    return res ? tobj : NULL;
}


void object_find_data_free(object_find_data **fd) {

    if (!*fd) {
        return;
    }

    tobject_match_list *cur = (*fd)->head;
    while (cur) {
        tobject_match_list *tmp = cur;
        cur = cur->next;
        free(tmp);
    }

    free(*fd);
    *fd = NULL;

    return;
}

static object_find_data *object_find_data_new(void) {
    return calloc(1, sizeof(object_find_data));
}

static CK_RV do_match_set(tobject_match_list *match_cur, tobject *tobj) {

    match_cur->tobj_handle = tobj->index;

    CK_ATTRIBUTE_PTR a = attr_get_attribute_by_type(tobj->attrs, CKA_CLASS);
    if (!a) {
        LOGE("Objects must have CK_OBJECT_CLASS");
        assert(0);
        return CKR_GENERAL_ERROR;
    }

    CK_OBJECT_CLASS objclass;
    CK_RV rv = attr_CK_ULONG(a, &objclass);
    if (rv != CKR_OK) {
        return rv;
    }

    match_cur->class = objclass;

    return CKR_OK;
}

CK_RV object_find_init(session_ctx *ctx, CK_ATTRIBUTE_PTR templ, CK_ULONG count) {

    // if count is 0 template is not used and all objects are requested so templ can be NULL.
    if (count > 0) {
        check_pointer(templ);
    }

    CK_RV rv = CKR_GENERAL_ERROR;

    object_find_data *fd = NULL;

    bool is_active = session_ctx_opdata_is_active(ctx);
    if (is_active) {
        rv = CKR_OPERATION_ACTIVE;
        goto out;
    }

    fd = object_find_data_new();
    if (!fd) {
        rv = CKR_HOST_MEMORY;
        goto out;
    }

    token *tok = session_ctx_get_token(ctx);
    assert(tok);

    if (!tok->tobjects.head) {
        goto empty;
    }

    tobject_match_list *match_cur = NULL;
    list *cur = &tok->tobjects.head->l;
    while(cur) {

        // Get the current object, and grab it's id for the object handle
        tobject *tobj = list_entry(cur, tobject, l);
        cur = cur->next;

        tobject *match = object_attr_filter(tobj, templ, count);
        if (!match) {
            continue;
        }

        /* we have a match, build the list */
        if (!fd->head) {
            /* set the head to point into the list */
            fd->head = calloc(1, sizeof(*match_cur));
            if (!fd->head) {
                rv = CKR_HOST_MEMORY;
                goto out;
            }

            match_cur = fd->head;

        } else {
            assert(match_cur);
            match_cur->next = calloc(1, sizeof(*match_cur));
            if (!match_cur->next) {
                rv = CKR_HOST_MEMORY;
                goto out;
            }

            match_cur = match_cur->next;
        }

        rv = do_match_set(match_cur, tobj);
        if (rv != CKR_OK) {
            goto out;
        }
    }

    fd->cur = fd->head;

empty:

    session_ctx_opdata_set(ctx, operation_find, NULL, fd, (opdata_free_fn)object_find_data_free);

    rv = CKR_OK;

out:
    if (rv != CKR_OK) {
        object_find_data_free(&fd);
    }

    return rv;
}

static bool is_not_public(tobject_match_list *match) {

    return match->class == CKO_SECRET_KEY || match->class == CKO_PRIVATE_KEY;
}

CK_RV object_find(session_ctx *ctx, CK_OBJECT_HANDLE *object, CK_ULONG max_object_count, CK_ULONG_PTR object_count) {

    check_pointer(object);
    check_pointer(object_count);

    CK_RV rv = CKR_OK;

    object_find_data *opdata = NULL;
    rv = session_ctx_opdata_get(ctx, operation_find, &opdata);
    if (rv != CKR_OK) {
        return rv;
    }

    token *tok = session_ctx_get_token(ctx);
    assert(tok);

    CK_ULONG count = 0;
    while(opdata->cur && count < max_object_count) {

        // Get the current object, and grab it's id for the object handle
        CK_OBJECT_HANDLE handle = opdata->cur->tobj_handle;

        // filter out CKO_PRIVATE and CKO_SECRET if not logged in
        if (!token_is_user_logged_in(tok) && is_not_public(opdata->cur)) {
            opdata->cur = opdata->cur->next;
            continue;
        }

        object[count] = handle;

        // Update our iterator
        opdata->cur = opdata->cur->next;

        count++;
    }

    *object_count = count;

    return CKR_OK;
}

CK_RV object_find_final(session_ctx *ctx) {

    CK_RV rv = CKR_GENERAL_ERROR;

    object_find_data *opdata = NULL;
    rv = session_ctx_opdata_get(ctx, operation_find, &opdata);
    if (rv != CKR_OK) {
        return rv;
    }

    session_ctx_opdata_clear(ctx);

    return CKR_OK;
}

CK_RV object_get_attributes(session_ctx *ctx, CK_OBJECT_HANDLE object, CK_ATTRIBUTE *templ, CK_ULONG count) {

    token *tok = session_ctx_get_token(ctx);
    assert(tok);

    tobject *tobj = NULL;
    CK_RV rv = token_find_tobject(tok, object, &tobj);
    if (rv != CKR_OK) {
        return rv;
    }

    rv = tobject_user_increment(tobj);
    if (rv != CKR_OK) {
        return rv;
    }

    /*
     * For each item requested in the template, find if the request has a match
     * and copy the size and possibly data (if allocated).
     */

    CK_ULONG i;
    for (i=0; i < count; i++) {

        CK_ATTRIBUTE_PTR t = &templ[i];

        CK_ATTRIBUTE_PTR found = attr_get_attribute_by_type(tobj->attrs, t->type);
        if (found) {
            if (!t->pValue) {
                /* only populate size if the buffer is null */
                t->ulValueLen = found->ulValueLen;
                continue;
            }

            /* The found attribute should fit inside the one to copy to */
            if (found->ulValueLen > t->ulValueLen) {
                t->ulValueLen = CK_UNAVAILABLE_INFORMATION;
                rv = CKR_BUFFER_TOO_SMALL;
                continue;
            }

            t->ulValueLen = found->ulValueLen;
            if (found->ulValueLen && found->pValue) {
                memcpy(t->pValue, found->pValue, found->ulValueLen);
            }
       } else {
           /* If it's not found it defaults to empty. */
           t->pValue = NULL;
           t->ulValueLen = CK_UNAVAILABLE_INFORMATION;
           LOGV("Invalid Attribute for tid %lu: type(%lu) ulValueLen(%lu), pData(%s)",
                   tobj->id, t->type, t->ulValueLen, t->pValue ? "non-null" : "null");
           rv = CKR_ATTRIBUTE_TYPE_INVALID;
       }
    }

    tobject_user_decrement(tobj);
    // if no error occurred rv is CKR_OK from previous call
    return rv;
}

tobject *tobject_new(void) {

    tobject *tobj = calloc(1, sizeof(tobject));
    if (!tobj) {
        LOGE("oom");
        return NULL;
    }

    return tobj;
}

CK_RV tobject_set_blob_data(tobject *tobj, twist pub, twist priv) {
    assert(pub);
    assert(tobj);

    tobj->priv = twist_dup(priv);
    if (priv && !tobj->priv) {
        LOGE("oom");
        return CKR_HOST_MEMORY;
    }

    tobj->pub = twist_dup(pub);
    if (!tobj->pub) {
        twist_free(tobj->priv);
        LOGE("oom");
        return CKR_HOST_MEMORY;
    }

    if (priv) {
        bool r = attr_list_add_buf(tobj->attrs, CKA_TPM2_PRIV_BLOB,
                (CK_BYTE_PTR)priv, twist_len(priv));
        if (!r) {
            return CKR_GENERAL_ERROR;
        }
    }

    bool r = attr_list_add_buf(tobj->attrs, CKA_TPM2_PUB_BLOB,
            (CK_BYTE_PTR)pub, pub ? twist_len(pub) : 0);

    return r ? CKR_OK : CKR_GENERAL_ERROR;
}

CK_RV tobject_set_auth(tobject *tobj, twist authbin, twist wrappedauthhex) {
    assert(tobj);
    assert(authbin);
    assert(wrappedauthhex);

    tobj->unsealed_auth = twist_dup(authbin);
    if (!tobj->unsealed_auth) {
        LOGE("oom");
        return CKR_HOST_MEMORY;
    }

    tobj->objauth = twist_dup(wrappedauthhex);
    if (!tobj->objauth) {
        LOGE("oom");
        twist_free(tobj->unsealed_auth);
        return CKR_HOST_MEMORY;
    }

    bool r = attr_list_add_buf(tobj->attrs, CKA_TPM2_OBJAUTH_ENC,
            (CK_BYTE_PTR)wrappedauthhex, twist_len(wrappedauthhex));
    return r ? CKR_OK : CKR_GENERAL_ERROR;
}

void tobject_set_handle(tobject *tobj, uint32_t handle) {
    assert(tobj);

    tobj->handle = handle;
}

void tobject_set_id(tobject *tobj, unsigned id) {
    assert(tobj);
    tobj->id = id;
}

attr_list *tobject_get_attrs(tobject *tobj) {
    return tobj->attrs;
}

CK_RV tobject_user_increment(tobject *tobj) {

    if (tobj->active == UINT_MAX) {
       LOGE("tobject active at max count, cannot issue. id: %lu", tobj->id);
       return CKR_GENERAL_ERROR;
    }

    tobj->active++;

    return CKR_OK;
}

CK_RV tobject_user_decrement(tobject *tobj) {

    if (!tobj->active) {
        LOGE("Returning a non-active tobject id: %lu", tobj->id);
        return CKR_GENERAL_ERROR;
    }

    tobj->active--;

    return CKR_OK;
}

static bool tobject_is_busy(tobject *tobj) {
    assert(tobj);

    return tobj->active > 0;
}

CK_RV object_destroy(session_ctx *ctx, CK_OBJECT_HANDLE object) {

    token *tok = session_ctx_get_token(ctx);
    assert(tok);

    tobject *tobj = NULL;
    CK_RV rv = token_find_tobject(tok, object, &tobj);
    if (rv != CKR_OK) {
        return rv;
    }

    bool is_busy = tobject_is_busy(tobj);
    if (is_busy) {
        return CKR_FUNCTION_FAILED;
    }

    rv = db_delete_object(tobj);
    if (rv != CKR_OK) {
        return rv;
    }

    token_rm_tobject(tok, tobj);

    tobject_free(tobj);
    rv = CKR_OK;

    return rv;
}

static CK_RV handle_rsa_public(token *tok, CK_ATTRIBUTE_PTR templ, CK_ULONG count, tobject **tobj) {

    CK_RV rv = CKR_GENERAL_ERROR;

    tobject *obj = NULL;

    /* RSA Public keys should have a modulus and exponent, verify */
    CK_ATTRIBUTE_PTR a_modulus = attr_get_attribute_by_type_raw(templ, count, CKA_MODULUS);
    if (!a_modulus) {
        return CKR_TEMPLATE_INCOMPLETE;
    }

    CK_ATTRIBUTE_PTR a_exponent = attr_get_attribute_by_type_raw(templ, count, CKA_PUBLIC_EXPONENT);
    if (!a_exponent) {
        return CKR_TEMPLATE_INCOMPLETE;
    }

    /*
     * Create a new typed attr list
     */
    attr_list *tmp_attrs = NULL;
    bool res = attr_typify(templ, count, &tmp_attrs);
    if (!res) {
        return CKR_GENERAL_ERROR;
    }
    assert(tmp_attrs);

    /* Add attribute CKA_LOCAL if missing */
    CK_ATTRIBUTE_PTR a = attr_get_attribute_by_type(tmp_attrs, CKA_LOCAL);
    if (!a) {
        bool result = attr_list_add_bool(tmp_attrs, CKA_LOCAL, CK_FALSE);
        if (!result) {
            rv = CKR_HOST_MEMORY;
            goto out;
        }
    }

    /* add CKA_ENCRYPT if missing */
    a = attr_get_attribute_by_type(tmp_attrs, CKA_ENCRYPT);
    if (!a) {
        bool result = attr_list_add_bool(tmp_attrs, CKA_ENCRYPT, CK_TRUE);
        if (!result) {
            rv = CKR_HOST_MEMORY;
            goto out;
        }
    }

    /* add CKA_VERIFY if missing */
    a = attr_get_attribute_by_type(tmp_attrs, CKA_VERIFY);
    if (!a) {
        bool result = attr_list_add_bool(tmp_attrs, CKA_VERIFY, CK_TRUE);
        if (!result) {
            rv = CKR_HOST_MEMORY;
            goto out;
        }
    }

    /* add CKA_KEY_GEN_MECHANISM as CK_UNAVAILABLE_INFORMATION */
    a = attr_get_attribute_by_type(tmp_attrs, CKA_KEY_GEN_MECHANISM);
    if (!a) {
        bool result = attr_list_add_int(tmp_attrs, CKA_KEY_GEN_MECHANISM, CK_UNAVAILABLE_INFORMATION);
        if (!result) {
            rv = CKR_HOST_MEMORY;
            goto out;
        }
    } else {
        CK_ULONG genmech = 0;
        rv = attr_CK_ULONG(a, &genmech);
        if (rv != CKR_OK) {
            LOGE("Error converting attribute CKA_KEY_GEN_MECHANISM");
            goto out;
        }

        if (genmech != CK_UNAVAILABLE_INFORMATION) {
           LOGE("CKA_KEY_GEN_MECHANISM cannot be anything but "
                   "CKA_KEY_GEN_MECHANISM, got: %lu",
                   genmech);
           rv = CKR_ATTRIBUTE_VALUE_INVALID;
           goto out;
        }
    }

    /* populate CKA_ALLOWED_MECHANISMS */
    rv = rsa_gen_mechs(tmp_attrs, NULL);
    if (rv != CKR_OK) {
        LOGE("Could not add RSA public mechanisms");
        goto out;
    }

    /* populate missing RSA public key attributes */
    rv = attr_common_add_RSA_publickey(&tmp_attrs);
    if (rv != CKR_OK) {
        LOGE("Could not add RSA public missing attributes");
        goto out;
    }

    /*
     * Now that everything is verified, create a new tobject
     * and populate the fields that matter.
     */
    obj = tobject_new();
    if (!obj) {
        LOGE("oom");
        rv = CKR_HOST_MEMORY;
        goto out;
    }

    obj->attrs = tmp_attrs;
    tmp_attrs = NULL;

    /* add the object to the db */
    rv = db_add_new_object(tok, obj);
    if (rv != CKR_OK) {
        goto out;
    }

    /* assign temp tobject to callee provided pointer */
    *tobj = obj;

    /* callee now takes owenership */
    obj = NULL;

    rv = CKR_OK;

out:
    attr_list_free(tmp_attrs);
    tobject_free(obj);

    return rv;
}

CK_RV object_create(session_ctx *ctx, CK_ATTRIBUTE *templ, CK_ULONG count, CK_OBJECT_HANDLE *object) {
    assert(ctx);
    check_pointer(templ);
    check_pointer(object);

    CK_RV rv = CKR_GENERAL_ERROR;

    CK_STATE state = session_ctx_state_get(ctx);
    LOGV("state: %lu", state);

    /*
     * Currently we only support RW user session state objects.
     */
    if (state != CKS_RW_USER_FUNCTIONS) {
        if (state == CKS_RW_SO_FUNCTIONS) {
            return CKR_USER_NOT_LOGGED_IN;
        } else {
            return CKR_SESSION_READ_ONLY;
        }
    }

    /*
     * If CKA_LOCAL is specified, it can never be CK_TRUE
     */
    CK_ATTRIBUTE_PTR a = attr_get_attribute_by_type_raw(templ, count, CKA_LOCAL);
    if (a) {
        CK_BBOOL bbool = CK_FALSE;
        rv = attr_CK_BBOOL(a, &bbool);
        if (rv != CKR_OK) {
            LOGE("Error converting attribute CKA_LOCAL");
            return rv;
        }
        if (bbool == CK_TRUE) {
           LOGE("CKA_LOCAL cannot be true");
           return CKR_ATTRIBUTE_VALUE_INVALID;
        }
    }

    /*
     * We only support RSA Public objects, so verify it.
     */
    a = attr_get_attribute_by_type_raw(templ, count, CKA_CLASS);
    if (!a) {
        LOGE("Expected attribute CKA_CLASS");
        return CKR_TEMPLATE_INCOMPLETE;
    }

    CK_OBJECT_CLASS clazz;
    rv = attr_CK_OBJECT_CLASS(a, &clazz);
    if (rv != CKR_OK) {
        LOGE("Error converting attribute CKA_CLASS");
        return CKR_TEMPLATE_INCOMPLETE;
    }

    a = attr_get_attribute_by_type_raw(templ, count, CKA_KEY_TYPE);
    if (!a) {
        LOGE("Expected attribute CKA_KEY_TYPE");
        return CKR_TEMPLATE_INCOMPLETE;
    }

    CK_KEY_TYPE key_type;
    rv = attr_CK_KEY_TYPE(a, &key_type);
    if (rv != CKR_OK) {
        LOGE("Error converting attribute CKA_KEY_TYPE");
        return rv;
    }

    if (key_type != CKK_RSA ||
            clazz != CKO_PUBLIC_KEY) {
        LOGE("Can only create RSA Public key objects, "
                "CKA_CLASS(%lu), CKA_KEY_TYPE(%lu)",
                clazz, key_type);
        return CKR_ATTRIBUTE_VALUE_INVALID;
    }

    token *tok = session_ctx_get_token(ctx);
    assert(tok);

    tobject *new_tobj = NULL;
    rv = handle_rsa_public(tok, templ, count, &new_tobj);
    if (rv != CKR_OK) {
        LOGE("Error creating rsa public key: %lu", rv);
        return rv;
    }

    rv = token_add_tobject(tok, new_tobj);
    if (rv != CKR_OK) {
        return rv;
    }

    *object = new_tobj->index;

    return CKR_OK;
}
