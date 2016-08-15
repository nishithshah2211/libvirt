/*
 * virsh-completers.h: Completers used for autocompletion
 *
 * Copyright (C) 2005, 2007-2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Michal Privoznik <mprivozn@redhat.com>
 * Nishith Shah <nishithshah.2211@gmail.com>
 *
 */

#include <config.h>
#include "virsh-completer.h"

#include "internal.h"
#include "viralloc.h"
#include "virstring.h"

#ifdef WITH_READLINE
/* vshCompleters - Option Completers */
vshCompleter vshDomainCompleter(unsigned int completer_flags, void *opaque)
{
    virDomainPtr *domains;
    char **names = NULL;
    int ndomains;
    size_t i;
    virshControlPtr priv = ((vshControl *)opaque)->privData;

    ndomains = virConnectListAllDomains(priv->conn, &domains, completer_flags);

    if (ndomains < 0)
        return NULL;

    if (VIR_ALLOC_N(names, ndomains+1) < 0)
        return NULL;

    for (i = 0; i < ndomains; i++) {
        const char *name = virDomainGetName(domains[i]);
        if (VIR_STRDUP(names[i], name) < 0)
            goto error;
        virDomainFree(domains[i]);
    }
    names[i] = NULL;
    VIR_FREE(domains);
    return (vshCompleter)names;

 error:
    virStringFreeList(names);
    for (; i < ndomains; i++)
        virDomainFree(domains[i]);
    VIR_FREE(domains);
    return NULL;
}

#else
vshCompleter vshDomainCompleter(unsigned int completer_flags ATTRIBUTE_UNUSED)
{
    return NULL;
}
#endif
