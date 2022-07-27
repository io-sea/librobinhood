#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/xattr.h>

#include <lustre/lustreapi.h>

#include "robinhood/backends/posix.h"
#include "robinhood/backends/posix_internal.h"
#include "robinhood/backends/lustre.h"

struct iterator_data {
    struct rbh_value *stripe_count;
    struct rbh_value *stripe_size;
    struct rbh_value *mirror_id;
    struct rbh_value *pattern;
    struct rbh_value *begin;
    struct rbh_value *flags;
    struct rbh_value *pool;
    struct rbh_value *end;
    struct rbh_value *ost;
    int comp_index;
    int ost_size;
    int ost_idx;
};

static __thread struct rbh_sstack *_values;
static __thread bool is_dir;
static __thread bool is_reg;
static __thread bool is_symlink;

static inline int
fill_pair(const char *key, const struct rbh_value *value,
          struct rbh_value_pair *pair)
{
    pair->key = key;
    pair->value = rbh_sstack_push(_values, value, sizeof(*value));
    if (pair->value == NULL)
        return -1;

    return 0;
}

static inline int
fill_binary_pair(const char *key, const void *data, const ssize_t len,
                 struct rbh_value_pair *pair)
{
    const struct rbh_value binary_value = {
        .type = RBH_VT_BINARY,
        .binary = {
            .data = rbh_sstack_push(_values, data, len),
            .size = len,
        },
    };

    if (binary_value.binary.data == NULL)
        return -1;

    return fill_pair(key, &binary_value, pair);
}

static inline int
fill_string_pair(const char *key, const char *str, const int len,
                 struct rbh_value_pair *pair)
{
    const struct rbh_value string_value = {
        .type = RBH_VT_STRING,
        .string = rbh_sstack_push(_values, str, len),
    };

    if (string_value.string == NULL)
        return -1;

    return fill_pair(key, &string_value, pair);
}

static inline int
fill_int32_pair(const char *key, int32_t integer, struct rbh_value_pair *pair)
{
    const struct rbh_value int32_value = {
        .type = RBH_VT_INT32,
        .int32 = integer,
    };

    return fill_pair(key, &int32_value, pair);
}

static inline int
fill_uint32_pair(const char *key, uint32_t integer, struct rbh_value_pair *pair)
{
    const struct rbh_value uint32_value = {
        .type = RBH_VT_UINT32,
        .uint32 = integer,
    };

    return fill_pair(key, &uint32_value, pair);
}

static inline int
fill_uint64_pair(const char *key, uint64_t integer, struct rbh_value_pair *pair)
{
    const struct rbh_value uint64_value = {
        .type = RBH_VT_UINT64,
        .uint64 = integer,
    };

    return fill_pair(key, &uint64_value, pair);
}

static inline int
fill_sequence_pair(const char *key, struct rbh_value *values, uint64_t length,
                   struct rbh_value_pair *pair)
{
    const struct rbh_value sequence_value = {
        .type = RBH_VT_SEQUENCE,
        .sequence = {
            .values = rbh_sstack_push(_values, values,
                                      length * sizeof(*values)),
            .count = length,
        },
    };

    if (sequence_value.sequence.values == NULL)
        return -1;

    return fill_pair(key, &sequence_value, pair);
}

/**
 * Record a file's fid in \p pairs
 *
 * @param fd        file descriptor to check
 * @param pairs     list of pairs to fill
 *
 * @return          number of filled \p pairs
 */
static int
xattrs_get_fid(int fd, struct rbh_value_pair *pairs)
{
    struct lu_fid fid;
    int rc;

    rc = llapi_fd2fid(fd, &fid);
    if (rc) {
        errno = -rc;
        return -1;
    }

    rc = fill_binary_pair("fid", &fid, sizeof(fid), pairs);

    return rc ? : 1;
}

/**
 * Record a file's hsm attributes (state and archive_id) in \p pairs
 *
 * @param fd        file descriptor to check
 * @param pairs     list of pairs to fill
 *
 * @return          number of filled \p pairs
 */
static int
xattrs_get_hsm(int fd, struct rbh_value_pair *pairs)
{
    struct hsm_user_state hus;
    int subcount = 0;
    int rc;

    if (!is_reg)
        return 0;

    rc = llapi_hsm_state_get_fd(fd, &hus);
    if (rc) {
        errno = -rc;
        return -1;
    }

    rc = fill_uint32_pair("hsm_state", hus.hus_states, &pairs[subcount++]);
    if (rc)
        return -1;

    rc = fill_uint32_pair("hsm_archive_id", hus.hus_archive_id,
                          &pairs[subcount++]);
    if (rc)
        return -1;

    return subcount;
}

static inline struct rbh_value
create_uint64_value(uint64_t integer)
{
    struct rbh_value val = {
        .type = RBH_VT_UINT64,
        .uint64 = integer,
    };

    return val;
}

static inline struct rbh_value
create_uint32_value(uint32_t integer)
{
    struct rbh_value val = {
        .type = RBH_VT_UINT32,
        .uint32 = integer,
    };

    return val;
}

static inline struct rbh_value
create_string_value(char *str, size_t len)
{
    struct rbh_value val = {
        .type = RBH_VT_STRING,
        .string = rbh_sstack_push(_values, str, len),
    };

    return val;
}

/**
 * Resize the OST array in \p data if the list of the current component's OSTs
 * cannot fit
 *
 * @param data      iterator_data structure with OST array to resize
 * @param index     index corresponding to the position to fill in the \p data
 *                  arrays
 *
 * @return          -1 if an error occured (and errno is set), 0 otherwise
 */
static int
iter_data_ost_try_resize(struct iterator_data *data, int ost_len)
{
    void *tmp;

    if (data->ost_idx + ost_len > data->ost_size) {
        tmp = realloc(data->ost,
                      (data->ost_size + ost_len) * sizeof(*data->ost));
        if (tmp == NULL) {
            errno = -ENOMEM;
            return -1;
        }

        data->ost = tmp;
        data->ost_size += ost_len;
    }

    return 0;
}

/**
 * Fill \p data using \p layout attributes
 *
 * @param layout    layout to retrieve the attributes from
 * @param data      iterator_data structure to fill
 * @param index     index corresponding to the position to fill in the \p data
 *                  arrays
 *
 * @return          -1 if an error occured (and errno is set), 0 otherwise
 */
static int
fill_iterator_data(struct llapi_layout *layout,
                   struct iterator_data *data, const int index)
{
    char pool_tmp[LOV_MAXPOOLNAME + 1];
    uint64_t stripe_count = 0;
    bool is_init_or_not_comp;
    uint32_t flags = 0;
    uint64_t tmp = 0;
    int ost_len;
    int rc;

    rc = llapi_layout_stripe_count_get(layout, &stripe_count);
    if (rc)
        return -1;

    data->stripe_count[index] = create_uint64_value(stripe_count);

    rc = llapi_layout_stripe_size_get(layout, &tmp);
    if (rc)
        return -1;

    data->stripe_size[index] = create_uint64_value(tmp);

    rc = llapi_layout_pattern_get(layout, &tmp);
    if (rc)
        return -1;

    data->pattern[index] = create_uint64_value(tmp);

    rc = llapi_layout_comp_flags_get(layout, &flags);
    if (rc)
        return -1;

    data->flags[index] = create_uint32_value(flags);

    rc = llapi_layout_pool_name_get(layout, pool_tmp, sizeof(pool_tmp));
    if (rc)
        return -1;

    data->pool[index] = create_string_value(pool_tmp, strlen(pool_tmp) + 1);

    is_init_or_not_comp = (flags == LCME_FL_INIT ||
                           !llapi_layout_is_composite(layout));
    ost_len = (is_init_or_not_comp ? stripe_count : 1);

    rc = iter_data_ost_try_resize(data, ost_len);
    if (rc)
        return rc;

    if (is_init_or_not_comp) {
        uint64_t i;

        for (i = 0; i < stripe_count; ++i) {
            rc = llapi_layout_ost_index_get(layout, i, &tmp);
            if (rc == -1 && errno == EINVAL)
                break;
            else if (rc)
                return rc;

            data->ost[data->ost_idx++] = create_uint64_value(tmp);
        }
    } else {
        data->ost[data->ost_idx++] = create_uint64_value(-1);
    }

    return 0;
}

/**
 * Callback function called by Lustre when iterating over a layout's components
 *
 * @param layout    layout of the file, with the current component set by Lustre
 * @param cbdata    void pointer pointing to callback parameters
 *
 * @return          -1 on error, 0 otherwise
 */
static int
xattrs_layout_iterator(struct llapi_layout *layout, void *cbdata)
{
    struct iterator_data *data = (struct iterator_data*) cbdata;
    uint32_t mirror_id;
    uint64_t begin;
    uint64_t end;
    int rc;

    rc = fill_iterator_data(layout, data, data->comp_index);
    if (rc)
        return -1;

    rc = llapi_layout_comp_extent_get(layout, &begin, &end);
    if (rc)
        return -1;

    data->begin[data->comp_index] = create_uint64_value(begin);
    data->end[data->comp_index] = create_uint64_value(end);

    rc = llapi_layout_mirror_id_get(layout, &mirror_id);
    if (rc)
        return -1;

    data->mirror_id[data->comp_index] = create_uint32_value(mirror_id);

    data->comp_index += 1;

    return 0;
}

static int
layout_get_nb_comp(struct llapi_layout *layout, uint32_t *nb_comp)
{
    int rc;

    rc = llapi_layout_comp_use(layout, LLAPI_LAYOUT_COMP_USE_LAST);
    if (rc)
        return -1;

    rc = llapi_layout_comp_id_get(layout, nb_comp);
    if (rc)
        return -1;

    rc = llapi_layout_comp_use(layout, LLAPI_LAYOUT_COMP_USE_FIRST);
    if (rc)
        return -1;

    return 0;
}

static int
init_iterator_data(struct iterator_data *data, const uint32_t length,
                   const int nb_xattrs)
{
    /**
     * We want to fetch 8 attributes: mirror_id, stripe_count, stripe_size,
     * pattern, begin, end, flags, pool.
     *
     * Additionnaly, we will keep the OSTs of each component in a separate list
     * because its length isn't fixed.
     */
    struct rbh_value *arr = calloc(nb_xattrs * length, sizeof(*arr));
    if (arr == NULL)
        return -1;

    data->stripe_count = arr;
    data->stripe_size = &arr[1 * length];
    data->pattern = &arr[2 * length];
    data->flags = &arr[3 * length];
    data->pool = &arr[4 * length];

    if (nb_xattrs >= 6) {
        data->mirror_id = &arr[5 * length];
        data->begin = &arr[6 * length];
        data->end = &arr[7 * length];
    }

    data->ost = malloc(length * sizeof(*data->ost));
    if (data->ost == NULL) {
        free(arr);
        errno = -ENOMEM;
        return -1;
    }

    data->ost_size = length;
    data->ost_idx = 0;
    data->comp_index = 0;

    return 0;
}

static int
xattrs_fill_layout(struct iterator_data *data, int nb_xattrs,
                   struct rbh_value_pair *pairs)
{
    struct rbh_value *values[] = {data->stripe_count, data->stripe_size,
                                  data->pattern, data->flags, data->pool,
                                  data->mirror_id, data->begin, data->end};
    char *keys[] = {"stripe_count", "stripe_size", "pattern", "comp_flags",
                    "pool", "mirror_id", "begin", "end"};
    int subcount = 0;
    int rc;

    for (int i = 0; i < nb_xattrs; ++i) {
        rc = fill_sequence_pair(keys[i], values[i], data->comp_index,
                                &pairs[subcount++]);
        if (rc)
            return -1;
    }

    rc = fill_sequence_pair("ost", data->ost, data->ost_idx,
                            &pairs[subcount++]);
    if (rc)
        return -1;

    return subcount;
}

/**
 * Record a file's magic number and layout generation in \p pairs
 *
 * @param fd        file descriptor to check
 * @param pairs     list of pairs to fill
 *
 * @return          number of filled \p pairs
 */
static int
xattrs_get_magic_and_gen(int fd, struct rbh_value_pair *pairs)
{
    char lov_buf[XATTR_SIZE_MAX];
    ssize_t xattr_size;
    uint32_t magic = 0;
    int magic_str_len;
    int subcount = 0;
    uint32_t gen = 0;
    char *magic_str;
    int rc;

    xattr_size = fgetxattr(fd, XATTR_LUSTRE_LOV, lov_buf, sizeof(lov_buf));
    if (xattr_size < 0)
        return -1;

    magic = ((struct lov_user_md *) lov_buf)->lmm_magic;

    switch (magic) {
    case LOV_USER_MAGIC_V1:
        magic_str = "LOV_USER_MAGIC_V1";
        magic_str_len = strlen(magic_str);
        gen = ((struct lov_user_md_v1 *) lov_buf)->lmm_layout_gen;
        break;
    case LOV_USER_MAGIC_COMP_V1:
        magic_str = "LOV_USER_MAGIC_COMP_V1";
        magic_str_len = strlen(magic_str);
        gen = ((struct lov_comp_md_v1 *) lov_buf)->lcm_layout_gen;
        break;
    case LOV_USER_MAGIC_SEL:
        magic_str = "LOV_USER_MAGIC_SEL";
        magic_str_len = strlen(magic_str);
        gen = ((struct lov_comp_md_v1 *) lov_buf)->lcm_layout_gen;
        break;
    case LOV_USER_MAGIC_V3:
        magic_str = "LOV_USER_MAGIC_V3";
        magic_str_len = strlen(magic_str);
        gen = ((struct lov_user_md_v3 *) lov_buf)->lmm_layout_gen;
        break;
    case LOV_USER_MAGIC_SPECIFIC:
        magic_str = "LOV_USER_MAGIC_SPECIFIC";
        magic_str_len = strlen(magic_str);
        gen = ((struct lov_user_md_v3 *) lov_buf)->lmm_layout_gen;
        break;
    case LOV_USER_MAGIC_FOREIGN:
        magic_str = "LOV_USER_MAGIC_FOREIGN";
        magic_str_len = strlen(magic_str);
        gen = -1;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    rc = fill_string_pair("magic", magic_str, magic_str_len + 1,
                          &pairs[subcount++]);
    if (rc)
        return -1;

    rc = fill_uint32_pair("gen", gen, &pairs[subcount++]);
    if (rc)
        return -1;

    return subcount;
}

/**
 * Record a file's layout attributes:
 *  - main flags
 *  - magic number and layout generation if the file is regular
 *  - mirror_count if the file is composite
 *  - per component:
 *    - stripe_count
 *    - stripe_size
 *    - pattern
 *    - component flags
 *    - pool
 *    - ost
 *    - if the file is composite, 3 more attributes:
 *      - mirror_id
 *      - begin
 *      - end
 *
 * @param fd        file descriptor to check
 * @param pairs     list of pairs to fill
 *
 * @return          number of filled \p pairs
 */
static int
xattrs_get_layout(int fd, struct rbh_value_pair *pairs)
{
    struct iterator_data data = { .comp_index = 0 };
    struct llapi_layout *layout;
    uint16_t mirror_count = 0;
    uint32_t nb_comp = 1;
    /**
     * There are 6 layout header components in total, but OST is in its own
     * list, so we only consider 5 attributes for the main data array allocation
     */
    int save_errno = 0;
    int nb_xattrs = 5;
    int subcount = 0;
    uint32_t flags;
    int rc;

    if (is_symlink)
        return 0;

    layout = llapi_layout_get_by_fd(fd, 0);
    if (layout == NULL)
        return -1;

    rc = llapi_layout_flags_get(layout, &flags);
    if (rc)
        goto err;

    rc = fill_uint32_pair("flags", flags, &pairs[subcount++]);
    if (rc)
        goto err;

    if (is_reg) {
        rc = xattrs_get_magic_and_gen(fd, &pairs[subcount]);
        if (rc < 0)
            goto err;

        subcount += rc;
    }

    if (llapi_layout_is_composite(layout)) {
        rc = llapi_layout_mirror_count_get(layout, &mirror_count);
        if (rc)
            goto err;

        rc = fill_uint32_pair("mirror_count", mirror_count, &pairs[subcount++]);
        if (rc)
            goto err;

        rc = layout_get_nb_comp(layout, &nb_comp);
        if (rc)
            goto err;

        /** The file is composite, so we add 3 more xattrs to the main alloc */
        nb_xattrs += 3;
    }

    rc = init_iterator_data(&data, nb_comp, nb_xattrs);
    if (rc)
        goto err;

    if (llapi_layout_is_composite(layout))
        rc = llapi_layout_comp_iterate(layout, &xattrs_layout_iterator, &data);
    else
        rc = fill_iterator_data(layout, &data, 0);

    if (rc)
        goto free_data;

    rc = xattrs_fill_layout(&data, nb_xattrs, &pairs[subcount]);
    if (rc < 0)
        goto free_data;

    subcount += rc;
    rc = 0;

free_data:
    save_errno = errno;
    free(data.stripe_count);
    free(data.ost);

err:
    save_errno = save_errno ? : errno;
    llapi_layout_free(layout);
    errno = save_errno;
    return rc ? rc : subcount;
}

static int
xattrs_get_mdt_info(int fd, struct rbh_value_pair *pairs)
{
    int subcount = 0;
    int rc = 0;

    if (is_dir) {
        struct lmv_user_md lum = {
            .lum_magic = LMV_USER_MAGIC,
        };
        struct rbh_value *mdt_idx;
        int save_errno = 0;

        rc = ioctl(fd, LL_IOC_LMV_GETSTRIPE, &lum);
        if (rc && errno == ENODATA)
            return 0;
        else if (rc)
            return rc;

        mdt_idx = calloc(lum.lum_stripe_count, sizeof(*mdt_idx));
        if (mdt_idx == NULL)
            return -1;

        for (int i = 0; i < lum.lum_stripe_count; i++)
            mdt_idx[i] = create_uint32_value(lum.lum_objects[i].lum_mds);

        rc = fill_sequence_pair("mdt_idx", mdt_idx, lum.lum_stripe_count,
                                &pairs[subcount++]);
        save_errno = errno;
        free(mdt_idx);
        errno = save_errno;
        if (rc)
            return -1;

        rc = fill_uint32_pair("mdt_hash", lum.lum_hash_type,
                              &pairs[subcount++]);
        if (rc)
            return -1;

        rc = fill_uint32_pair("mdt_count", lum.lum_stripe_count,
                              &pairs[subcount++]);
        if (rc)
            return -1;
    } else if (!is_symlink) {
        int32_t mdt;

        rc = llapi_file_fget_mdtidx(fd, &mdt);
        if (rc)
            return -1;

        rc = fill_int32_pair("mdt_index", mdt, &pairs[subcount++]);
        if (rc)
            return -1;
    }

    return subcount;
}

#define XATTR_CCC_EXPIRES_AT "user.ccc_expires_at"
#define UINT64_MAX_STR_LEN 22

static void
xattrs_get_retention()
{
    struct rbh_value_pair new_pair;
    uint64_t result;
    char *end;

    for (int i = 0; i < *_inode_xattrs_count; ++i) {
        char tmp[UINT64_MAX_STR_LEN];

        if (strcmp(_inode_xattrs[i].key, XATTR_CCC_EXPIRES_AT) ||
            _inode_xattrs[i].value->binary.size >= UINT64_MAX_STR_LEN)
            continue;

        memcpy(tmp, _inode_xattrs[i].value->binary.data,
               _inode_xattrs[i].value->binary.size);
        tmp[_inode_xattrs[i].value->binary.size] = 0;

        result = strtoul(tmp, &end, 10);
        if (errno || (!result && tmp == end) || *end != '\0')
            break;

        fill_uint64_pair(_inode_xattrs[i].key, result, &new_pair);
        _inode_xattrs[i] = new_pair;
        break;
    }
}

static ssize_t
lustre_ns_xattrs_callback(const int fd, const uint16_t mode,
                          struct rbh_value_pair *pairs,
                          struct rbh_sstack *values)
{
    int (*xattrs_funcs[])(int, struct rbh_value_pair *) = {
        xattrs_get_fid, xattrs_get_hsm, xattrs_get_layout, xattrs_get_mdt_info
    };
    int count = 0;
    int subcount;

    is_symlink = S_ISLNK(mode);
    is_dir = S_ISDIR(mode);
    is_reg = S_ISREG(mode);
    _values = values;

    for (int i = 0; i < sizeof(xattrs_funcs) / sizeof(xattrs_funcs[0]); ++i) {
        subcount = xattrs_funcs[i](fd, &pairs[count]);
        if (subcount == -1)
            return -1;

        count += subcount;
    }

    xattrs_get_retention();

    return count;
}

struct posix_iterator *
lustre_iterator_new(const char *root, const char *entry, int statx_sync_type)
{
    struct posix_iterator *lustre_iter;

    lustre_iter = posix_iterator_new(root, entry, statx_sync_type);
    if (lustre_iter == NULL)
        return NULL;

    lustre_iter->ns_xattrs_callback = lustre_ns_xattrs_callback;

    return lustre_iter;
}

struct rbh_backend *
rbh_lustre_backend_new(const char *path)
{
    struct posix_backend *lustre;

    lustre = (struct posix_backend *)rbh_posix_backend_new(path);
    if (lustre == NULL)
        return NULL;

    lustre->iter_new = lustre_iterator_new;
    lustre->backend.id = RBH_BI_LUSTRE;
    lustre->backend.name = RBH_LUSTRE_BACKEND_NAME;

    return &lustre->backend;
}
