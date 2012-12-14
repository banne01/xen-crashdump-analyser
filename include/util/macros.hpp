/*
 *  This file is part of the Xen Crashdump Analyser.
 *
 *  The Xen Crashdump Analyser is free software: you can redistribute
 *  it and/or modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation, either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  The Xen Crashdump Analyser is distributed in the hope that it will
 *  be useful, but WITHOUT ANY WARRANTY; without even the implied
 *  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with the Xen Crashdump Analyser.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 *  Copyright (c) 2012 Citrix Inc.
 */

#ifndef __MACROS_HPP__
#define __MACROS_HPP__

/**
 * @file include/util/macros.hpp
 * @author Andrew Cooper
 */

// Header files included because of function calls in the macros
#include <cstdio>
#include <cstring>
#include <cerrno>

/// Safe delete an object
#define SAFE_DELETE(o) do { if ((o)) delete (o); (o) = NULL; } while (0)

/// Safe delete an array
#define SAFE_DELETE_ARRAY(a) do { if ((a)) delete [] (a); (a) = NULL; } while (0)

/**
 * Log an error in the case that fclose has failed.
 * For the common case (-ENOSPC), log only once to prevent console spam.
 * @param err errno from fclose().
 */
void fclose_failure(int err);

/**
 * Safe fclose a FILE.
 * Logs an error in the case that the call to fclose() is not successful.
 */
#define SAFE_FCLOSE(f)                      \
    do {                                    \
        if ((f) && (0 != fclose(f)) )       \
            fclose_failure(errno);          \
        (f) = NULL;                         \
    } while (0)

/**
 * Common catch statements.  Presented as a macro only for code brevity in
 * otherwise large and complex functions.
 */
#define CATCH_COMMON                                                    \
    catch ( const CommonError & e )                                     \
    {                                                                   \
        e.log();                                                        \
    }                                                                   \


/**
 * Catch filewrite exceptions.  Separate from CATCH_COMMON as only
 * certain pieces of code can sensibly deal with file errors.  It is not
 * neceserally safe to close file here, due to interactions with
 * set_additional_log(), so the user of this macro is required to close
 * the file.
 * @param n File name string
 */
#define CATCH_FILEWRITE(n)                                              \
    catch ( const filewrite & e )                                       \
    {                                                                   \
        e.log((n));                                                     \
    }


#endif

/*
 * Local variables:
 * mode: C++
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
