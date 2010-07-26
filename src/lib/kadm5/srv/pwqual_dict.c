/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Copyright 1993 OpenVision Technologies, Inc., All Rights Reserved
 *
 * $Header$
 */

/* Password quality module to look up paswords within the realm dictionary. */

#if !defined(lint) && !defined(__CODECENTER__)
static char *rcsid = "$Header$";
#endif

#include "k5-platform.h"
#include <krb5/pwqual_plugin.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <kadm5/admin.h>
#include "adm_proto.h"
#include <syslog.h>
#include "server_internal.h"

typedef struct dict_moddata_st {
    char **word_list;        /* list of word pointers */
    char *word_block;        /* actual word data */
    unsigned int word_count; /* number of words */
} *dict_moddata;


/*
 * Function: word_compare
 *
 * Purpose: compare two words in the dictionary.
 *
 * Arguments:
 *      w1              (input) pointer to first word
 *      w2              (input) pointer to second word
 *      <return value>  result of strcmp
 *
 * Requires:
 *      w1 and w2 to point to valid memory
 *
 */

static int
word_compare(const void *s1, const void *s2)
{
    return (strcasecmp(*(const char **)s1, *(const char **)s2));
}

/*
 * Function: init-dict
 *
 * Purpose: Initialize in memory word dictionary
 *
 * Arguments:
 *          none
 *          <return value> KADM5_OK on success errno on failure;
 *                         (but success on ENOENT)
 *
 * Requires:
 *      If WORDFILE exists, it must contain a list of words,
 *      one word per-line.
 *
 * Effects:
 *      If WORDFILE exists, it is read into memory sorted for future
 * use.  If it does not exist, it syslogs an error message and returns
 * success.
 *
 * Modifies:
 *      word_list to point to a chunck of allocated memory containing
 *      pointers to words
 *      word_block to contain the dictionary.
 *
 */

static int
init_dict(dict_moddata dict, const char *dict_file)
{
    int fd;
    size_t len, i;
    char *p, *t;
    struct stat sb;

    if (dict_file == NULL) {
        krb5_klog_syslog(LOG_INFO, "No dictionary file specified, continuing "
                         "without one.");
        return KADM5_OK;
    }
    if ((fd = open(dict_file, O_RDONLY)) == -1) {
        if (errno == ENOENT) {
            krb5_klog_syslog(LOG_ERR,
                             "WARNING!  Cannot find dictionary file %s, "
                             "continuing without one.", dict_file);
            return KADM5_OK;
        } else
            return errno;
    }
    set_cloexec_fd(fd);
    if (fstat(fd, &sb) == -1) {
        close(fd);
        return errno;
    }
    if ((dict->word_block = malloc(sb.st_size + 1)) == NULL)
        return ENOMEM;
    if (read(fd, dict->word_block, sb.st_size) != sb.st_size)
        return errno;
    (void) close(fd);
    dict->word_block[sb.st_size] = '\0';

    p = dict->word_block;
    len = sb.st_size;
    while(len > 0 && (t = memchr(p, '\n', len)) != NULL) {
        *t = '\0';
        len -= t - p + 1;
        p = t + 1;
        dict->word_count++;
    }
    if ((dict->word_list = malloc(dict->word_count * sizeof(char *))) == NULL)
        return ENOMEM;
    p = dict->word_block;
    for (i = 0; i < dict->word_count; i++) {
        dict->word_list[i] = p;
        p += strlen(p) + 1;
    }
    qsort(dict->word_list, dict->word_count, sizeof(char *), word_compare);
    return KADM5_OK;
}

/*
 * Function: destroy_dict
 *
 * Purpose: destroy in-core copy of dictionary.
 *
 * Arguments:
 *          none
 *          <return value>  none
 * Requires:
 *          nothing
 * Effects:
 *      frees up memory occupied by word_list and word_block
 *      sets count back to 0, and resets the pointers to NULL
 *
 * Modifies:
 *      word_list, word_block, and word_count.
 *
 */

static void
destroy_dict(dict_moddata dict)
{
    if (dict == NULL)
        return;
    free(dict->word_list);
    free(dict->word_block);
    free(dict);
    return;
}

/* Implement the password quality open method by reading in dict_file. */
static krb5_error_code
dict_open(krb5_context context, const char *dict_file,
          krb5_pwqual_moddata *data)
{
    krb5_error_code ret;
    dict_moddata dict;

    *data = NULL;

    /* Allocate and initialize a dictionary structure. */
    dict = malloc(sizeof(*dict));
    if (dict == NULL)
        return ENOMEM;
    dict->word_list = NULL;
    dict->word_block = NULL;
    dict->word_count = 0;

    /* Fill in the dictionary structure with data from dict_file. */
    ret = init_dict(dict, dict_file);
    if (ret != 0) {
        destroy_dict(dict);
        return ret;
    }

    *data = (krb5_pwqual_moddata)dict;
    return 0;
}

/* Implement the password quality check method by checking the password
 * against the dictionary, as well as against principal components. */
static krb5_error_code
dict_check(krb5_context context, krb5_pwqual_moddata data,
           const char *password, kadm5_policy_ent_t policy,
           krb5_principal princ)
{
    dict_moddata dict = (dict_moddata)data;
    int i, n;
    char *cp;

    /* Don't check the dictionary for principals with no password policy. */
    if (policy == NULL)
        return 0;

    /* Check against words in the dictionary if we successfully loaded one. */
    if (dict->word_list != NULL &&
        bsearch(&password, dict->word_list, dict->word_count, sizeof(char *),
                word_compare) != NULL)
        return KADM5_PASS_Q_DICT;

    /* Check against components of the principal. */
    n = krb5_princ_size(handle->context, princ);
    cp = krb5_princ_realm(handle->context, princ)->data;
    if (strcasecmp(cp, password) == 0)
	return KADM5_PASS_Q_DICT;
    for (i = 0; i < n; i++) {
	cp = krb5_princ_component(handle->context, princ, i)->data;
	if (strcasecmp(cp, password) == 0)
	    return KADM5_PASS_Q_DICT;
    }
    return 0;
}

/* Implement the password quality close method. */
static void
dict_close(krb5_context context, krb5_pwqual_moddata data)
{
    destroy_dict((dict_moddata)data);
}

krb5_error_code
pwqual_dict_initvt(krb5_context context, int maj_ver, int min_ver,
                   krb5_plugin_vtable vtable)
{
    krb5_pwqual_vtable vt;

    if (maj_ver != 1)
        return EINVAL; /* XXX create error code */
    vt = (krb5_pwqual_vtable)vtable;
    vt->open = dict_open;
    vt->check = dict_check;
    vt->close = dict_close;
    return 0;
}