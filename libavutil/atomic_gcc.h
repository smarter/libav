/*
 * Copyright (c) 2012 Ronald S. Bultje <rsbultje@gmail.com>
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "atomic.h"

#define avpriv_atomic_int_get atomic_int_get_gcc
static inline int atomic_int_get_gcc(volatile int *ptr)
{
    __sync_synchronize();
    return *ptr;
}

#define avpriv_atomic_int_set atomic_int_set_gcc
static inline void atomic_int_set_gcc(volatile int *ptr, int val)
{
    *ptr = val;
    __sync_synchronize();
}

#define avpriv_atomic_int_add_and_fetch atomic_int_add_and_fetch_gcc
static inline int atomic_int_add_and_fetch_gcc(volatile int *ptr, int inc)
{
    return __sync_add_and_fetch(ptr, inc);
}

#define avpriv_atomic_ptr_cas atomic_ptr_cas_gcc
static inline void *atomic_ptr_cas_gcc(void * volatile *ptr,
                                       void *oldval, void *newval)
{
    return __sync_val_compare_and_swap(ptr, oldval, newval);
}
