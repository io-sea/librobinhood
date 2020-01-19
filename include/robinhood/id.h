/* This file is part of the RobinHood Library
 * Copyright (C) 2019 Commissariat a l'energie atomique et aux energies
 *                    alternatives
 *
 * SPDX-License-Identifer: LGPL-3.0-or-later
 *
 * author: Quentin Bouget <quentin.bouget@cea.fr>
 */

#ifndef ROBINHOOD_ID_H
#define ROBINHOOD_ID_H

#include <stddef.h>

/** @file
 * IDs uniquely indentify the fsentries of a given filesystem.
 */

/**
 * A unique identifier for an fsentry
 *
 * An ID is a generic data type that can contain arbitrary data.
 *
 * They are used to uniquely identify fsentries throughout a filesystem's life.
 *
 * IDs have two fields:
 *   - \c data: a pointer to arbitrary data;
 *   - \c size: the number of bytes data points at
 *
 * As a convention an ID with a \c size of 0 is used to represent a filesystem
 * root's parent fsentry (something that does not exist).
 *
 * IDs are generally (if not always) built from file handles (cf.
 * name_to_handle_at(2)) or equivalents.
 */
struct rbh_id {
    const char *data;
    size_t size;
};

/**
 * Copy a struct rbh_id and its content
 *
 * @param dest      the ID to copy to
 * @param src       the ID to copy from
 * @param buffer    a pointer to a \p bufsize long buffer. It is used to store
 *                  the content of \p src (ie. src->data). On success, it is
 *                  updated to point after the data that was copied in it.
 * @param bufsize   a pointer to the number of bytes in \p buffer. On success,
 *                  its content is decremented by the number of bytes that were
 *                  written to \p buffer.
 *
 * @return          0 on success, -1 on error and errno is set appropriately
 *
 * @error ENOBUFS   \p bufsize is less than the amount of data in \p src
 *                  (ie. \c bufsize < \c src->id)
 */
int
rbh_id_copy(struct rbh_id *dest, const struct rbh_id *src, char **buffer,
            size_t *bufsize);

/**
 * Create a new struct rbh_id
 *
 * @param data      a pointer to \p size bytes of arbitrary data
 * @param size      the number of bytes \p data points at
 *
 * @return          a pointer to a newly allocated struct rbh_id on success,
 *                  NULL on error and errno is set appropriately
 *
 * @error ENOMEM    not enough memory available
 *
 * The returned ID points at an internally managed copy of \p data.
 */
struct rbh_id *
rbh_id_new(const char *data, size_t size);

struct file_handle;

/**
 * Create a new struct rbh_id from a struct file_handle
 *
 * @param handle    the file handle to use
 *
 * @return          a pointer to a newly allocated struct rbh_id on success,
 *                  NULL on error and errno is set appropriately
 *
 * @error ENOMEM    not enough memory available
 *
 * The returned ID does not share data with \p handle.
 */
struct rbh_id *
rbh_id_from_file_handle(const struct file_handle *handle);

struct lu_fid;

/**
 * Create a new struct rbh_id from a struct lu_fid
 *
 * @param fid       the Lustre fid to use
 *
 * @return          a pointer to a newly allocated struct rbh_id on success,
 *                  NULL on error and errno is set appropriately
 *
 * @error ENOMEM    not enough memory available
 *
 * The returned ID and \p fid do not share data.
 */
struct rbh_id *
rbh_id_from_lu_fid(const struct lu_fid *fid);

#endif
