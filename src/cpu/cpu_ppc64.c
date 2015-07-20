/*
 * cpu_ppc64.c: CPU driver for 64-bit PowerPC CPUs
 *
 * Copyright (C) 2013 Red Hat, Inc.
 * Copyright (C) IBM Corporation, 2010
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
 * Authors:
 *      Anton Blanchard <anton@au.ibm.com>
 *      Prerna Saxena <prerna@linux.vnet.ibm.com>
 *      Li Zhang <zhlcindy@linux.vnet.ibm.com>
 */

#include <config.h>
#include <stdint.h>

#include "virlog.h"
#include "viralloc.h"
#include "cpu.h"
#include "virstring.h"
#include "cpu_map.h"
#include "virbuffer.h"

#define VIR_FROM_THIS VIR_FROM_CPU

VIR_LOG_INIT("cpu.cpu_ppc64");

static const virArch archs[] = { VIR_ARCH_PPC64, VIR_ARCH_PPC64LE };

struct ppc64_vendor {
    char *name;
    struct ppc64_vendor *next;
};

struct ppc64_model {
    char *name;
    const struct ppc64_vendor *vendor;
    struct cpuPPC64Data data;
    struct ppc64_model *next;
};

struct ppc64_map {
    struct ppc64_vendor *vendors;
    struct ppc64_model *models;
};


static void
ppc64ModelFree(struct ppc64_model *model)
{
    if (model == NULL)
        return;

    VIR_FREE(model->name);
    VIR_FREE(model);
}

static struct ppc64_model *
ppc64ModelFind(const struct ppc64_map *map,
               const char *name)
{
    struct ppc64_model *model;

    model = map->models;
    while (model != NULL) {
        if (STREQ(model->name, name))
            return model;

        model = model->next;
    }

    return NULL;
}

static struct ppc64_model *
ppc64ModelFindPVR(const struct ppc64_map *map,
                  uint32_t pvr)
{
    struct ppc64_model *model;

    model = map->models;
    while (model != NULL) {
        if (model->data.pvr == pvr)
            return model;

        model = model->next;
    }

    /* PowerPC Processor Version Register is interpreted as follows :
     * Higher order 16 bits : Power ISA generation.
     * Lower order 16 bits : CPU chip version number.
     * If the exact CPU isn't found, return the nearest matching CPU generation
     */
    if (pvr & 0x0000FFFFul)
        return ppc64ModelFindPVR(map, (pvr & 0xFFFF0000ul));

    return NULL;
}

static struct ppc64_model *
ppc64ModelCopy(const struct ppc64_model *model)
{
    struct ppc64_model *copy;

    if (VIR_ALLOC(copy) < 0 ||
        VIR_STRDUP(copy->name, model->name) < 0) {
        ppc64ModelFree(copy);
        return NULL;
    }

    copy->data.pvr = model->data.pvr;
    copy->vendor = model->vendor;

    return copy;
}

static struct ppc64_vendor *
ppc64VendorFind(const struct ppc64_map *map,
                const char *name)
{
    struct ppc64_vendor *vendor;

    vendor = map->vendors;
    while (vendor) {
        if (STREQ(vendor->name, name))
            return vendor;

        vendor = vendor->next;
    }

    return NULL;
}

static void
ppc64VendorFree(struct ppc64_vendor *vendor)
{
    if (!vendor)
        return;

    VIR_FREE(vendor->name);
    VIR_FREE(vendor);
}

static struct ppc64_model *
ppc64ModelFromCPU(const virCPUDef *cpu,
                  const struct ppc64_map *map)
{
    struct ppc64_model *model = NULL;

    if ((model = ppc64ModelFind(map, cpu->model)) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown CPU model %s"), cpu->model);
        goto error;
    }

    if ((model = ppc64ModelCopy(model)) == NULL)
        goto error;

    return model;

 error:
    ppc64ModelFree(model);
    return NULL;
}


static int
ppc64VendorLoad(xmlXPathContextPtr ctxt,
                struct ppc64_map *map)
{
    struct ppc64_vendor *vendor = NULL;

    if (VIR_ALLOC(vendor) < 0)
        return -1;

    vendor->name = virXPathString("string(@name)", ctxt);
    if (!vendor->name) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Missing CPU vendor name"));
        goto ignore;
    }

    if (ppc64VendorFind(map, vendor->name)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU vendor %s already defined"), vendor->name);
        goto ignore;
    }

    if (!map->vendors) {
        map->vendors = vendor;
    } else {
        vendor->next = map->vendors;
        map->vendors = vendor;
    }

 cleanup:
    return 0;

 ignore:
    ppc64VendorFree(vendor);
    goto cleanup;
}

static int
ppc64ModelLoad(xmlXPathContextPtr ctxt,
               struct ppc64_map *map)
{
    struct ppc64_model *model;
    char *vendor = NULL;
    unsigned long pvr;

    if (VIR_ALLOC(model) < 0)
        return -1;

    model->name = virXPathString("string(@name)", ctxt);
    if (!model->name) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("Missing CPU model name"));
        goto ignore;
    }

    if (ppc64ModelFind(map, model->name)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("CPU model %s already defined"), model->name);
        goto ignore;
    }

    if (virXPathBoolean("boolean(./vendor)", ctxt)) {
        vendor = virXPathString("string(./vendor/@name)", ctxt);
        if (!vendor) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Invalid vendor element in CPU model %s"),
                           model->name);
            goto ignore;
        }

        if (!(model->vendor = ppc64VendorFind(map, vendor))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown vendor %s referenced by CPU model %s"),
                           vendor, model->name);
            goto ignore;
        }
    }

    if (!virXPathBoolean("boolean(./pvr)", ctxt) ||
        virXPathULongHex("string(./pvr/@value)", ctxt, &pvr) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Missing or invalid PVR value in CPU model %s"),
                       model->name);
        goto ignore;
    }
    model->data.pvr = pvr;

    if (map->models == NULL) {
        map->models = model;
    } else {
        model->next = map->models;
        map->models = model;
    }

 cleanup:
    VIR_FREE(vendor);
    return 0;

 ignore:
    ppc64ModelFree(model);
    goto cleanup;
}

static int
ppc64MapLoadCallback(cpuMapElement element,
                     xmlXPathContextPtr ctxt,
                     void *data)
{
    struct ppc64_map *map = data;

    switch (element) {
    case CPU_MAP_ELEMENT_VENDOR:
        return ppc64VendorLoad(ctxt, map);
    case CPU_MAP_ELEMENT_MODEL:
        return ppc64ModelLoad(ctxt, map);
    case CPU_MAP_ELEMENT_FEATURE:
    case CPU_MAP_ELEMENT_LAST:
        break;
    }

    return 0;
}

static void
ppc64MapFree(struct ppc64_map *map)
{
    if (map == NULL)
        return;

    while (map->models != NULL) {
        struct ppc64_model *model = map->models;
        map->models = model->next;
        ppc64ModelFree(model);
    }

    while (map->vendors != NULL) {
        struct ppc64_vendor *vendor = map->vendors;
        map->vendors = vendor->next;
        ppc64VendorFree(vendor);
    }

    VIR_FREE(map);
}

static struct ppc64_map *
ppc64LoadMap(void)
{
    struct ppc64_map *map;

    if (VIR_ALLOC(map) < 0)
        return NULL;

    if (cpuMapLoad("ppc64", ppc64MapLoadCallback, map) < 0)
        goto error;

    return map;

 error:
    ppc64MapFree(map);
    return NULL;
}

static virCPUDataPtr
ppc64MakeCPUData(virArch arch,
                 struct cpuPPC64Data *data)
{
    virCPUDataPtr cpuData;

    if (VIR_ALLOC(cpuData) < 0)
        return NULL;

    cpuData->arch = arch;
    cpuData->data.ppc64 = *data;
    data = NULL;

    return cpuData;
}

static virCPUCompareResult
ppc64Compute(virCPUDefPtr host,
             const virCPUDef *cpu,
             virCPUDataPtr *guestData,
             char **message)

{
    struct ppc64_map *map = NULL;
    struct ppc64_model *host_model = NULL;
    struct ppc64_model *guest_model = NULL;

    virCPUCompareResult ret = VIR_CPU_COMPARE_ERROR;
    virArch arch;
    size_t i;

    if (cpu->arch != VIR_ARCH_NONE) {
        bool found = false;

        for (i = 0; i < ARRAY_CARDINALITY(archs); i++) {
            if (archs[i] == cpu->arch) {
                found = true;
                break;
            }
        }

        if (!found) {
            VIR_DEBUG("CPU arch %s does not match host arch",
                      virArchToString(cpu->arch));
            if (message &&
                virAsprintf(message,
                            _("CPU arch %s does not match host arch"),
                            virArchToString(cpu->arch)) < 0)
                goto cleanup;

            ret = VIR_CPU_COMPARE_INCOMPATIBLE;
            goto cleanup;
        }
        arch = cpu->arch;
    } else {
        arch = host->arch;
    }

    if (cpu->vendor &&
        (!host->vendor || STRNEQ(cpu->vendor, host->vendor))) {
        VIR_DEBUG("host CPU vendor does not match required CPU vendor %s",
                  cpu->vendor);
        if (message &&
            virAsprintf(message,
                        _("host CPU vendor does not match required "
                        "CPU vendor %s"),
                        cpu->vendor) < 0)
            goto cleanup;

        ret = VIR_CPU_COMPARE_INCOMPATIBLE;
        goto cleanup;
    }

    if (!(map = ppc64LoadMap()) ||
        !(host_model = ppc64ModelFromCPU(host, map)) ||
        !(guest_model = ppc64ModelFromCPU(cpu, map)))
        goto cleanup;

    if (guestData != NULL) {
        if (cpu->type == VIR_CPU_TYPE_GUEST &&
            cpu->match == VIR_CPU_MATCH_STRICT &&
            STRNEQ(guest_model->name, host_model->name)) {
            VIR_DEBUG("host CPU model does not match required CPU model %s",
                      guest_model->name);
            if (message &&
                virAsprintf(message,
                            _("host CPU model does not match required "
                            "CPU model %s"),
                            guest_model->name) < 0)
                goto cleanup;

            ret = VIR_CPU_COMPARE_INCOMPATIBLE;
            goto cleanup;
        }

        if (!(*guestData = ppc64MakeCPUData(arch, &guest_model->data)))
            goto cleanup;
    }

    ret = VIR_CPU_COMPARE_IDENTICAL;

 cleanup:
    ppc64MapFree(map);
    ppc64ModelFree(host_model);
    ppc64ModelFree(guest_model);
    return ret;
}

static virCPUCompareResult
ppc64Compare(virCPUDefPtr host,
             virCPUDefPtr cpu,
             bool failIncompatible)
{
    if ((cpu->arch == VIR_ARCH_NONE || host->arch == cpu->arch) &&
        STREQ(host->model, cpu->model))
        return VIR_CPU_COMPARE_IDENTICAL;

    if (failIncompatible) {
        virReportError(VIR_ERR_CPU_INCOMPATIBLE, NULL);
        return VIR_CPU_COMPARE_ERROR;
    } else {
        return VIR_CPU_COMPARE_INCOMPATIBLE;
    }
}

static int
ppc64Decode(virCPUDefPtr cpu,
            const virCPUData *data,
            const char **models,
            unsigned int nmodels,
            const char *preferred ATTRIBUTE_UNUSED,
            unsigned int flags)
{
    int ret = -1;
    struct ppc64_map *map;
    const struct ppc64_model *model;

    virCheckFlags(VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES, -1);

    if (data == NULL || (map = ppc64LoadMap()) == NULL)
        return -1;

    if (!(model = ppc64ModelFindPVR(map, data->data.ppc64.pvr))) {
        virReportError(VIR_ERR_OPERATION_FAILED,
                       _("Cannot find CPU model with PVR 0x%08x"),
                       data->data.ppc64.pvr);
        goto cleanup;
    }

    if (!cpuModelIsAllowed(model->name, models, nmodels)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("CPU model %s is not supported by hypervisor"),
                       model->name);
        goto cleanup;
    }

    if (VIR_STRDUP(cpu->model, model->name) < 0 ||
        (model->vendor && VIR_STRDUP(cpu->vendor, model->vendor->name) < 0)) {
        goto cleanup;
    }

    ret = 0;

 cleanup:
    ppc64MapFree(map);

    return ret;
}


static void
ppc64DataFree(virCPUDataPtr data)
{
    if (data == NULL)
        return;

    VIR_FREE(data);
}

static virCPUDataPtr
ppc64NodeData(virArch arch)
{
    virCPUDataPtr cpuData;

    if (VIR_ALLOC(cpuData) < 0)
        return NULL;

    cpuData->arch = arch;

#if defined(__powerpc__) || defined(__powerpc64__)
    asm("mfpvr %0"
        : "=r" (cpuData->data.ppc64.pvr));
#endif

    return cpuData;
}

static virCPUCompareResult
ppc64GuestData(virCPUDefPtr host,
               virCPUDefPtr guest,
               virCPUDataPtr *data,
               char **message)
{
    return ppc64Compute(host, guest, data, message);
}

static int
ppc64Update(virCPUDefPtr guest,
            const virCPUDef *host)
{
    switch ((virCPUMode) guest->mode) {
    case VIR_CPU_MODE_HOST_MODEL:
    case VIR_CPU_MODE_HOST_PASSTHROUGH:
        guest->match = VIR_CPU_MATCH_EXACT;
        virCPUDefFreeModel(guest);
        return virCPUDefCopyModel(guest, host, true);

    case VIR_CPU_MODE_CUSTOM:
        return 0;

    case VIR_CPU_MODE_LAST:
        break;
    }

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("Unexpected CPU mode: %d"), guest->mode);
    return -1;
}

static virCPUDefPtr
ppc64Baseline(virCPUDefPtr *cpus,
              unsigned int ncpus,
              const char **models ATTRIBUTE_UNUSED,
              unsigned int nmodels ATTRIBUTE_UNUSED,
              unsigned int flags)
{
    struct ppc64_map *map = NULL;
    const struct ppc64_model *model;
    const struct ppc64_vendor *vendor = NULL;
    virCPUDefPtr cpu = NULL;
    size_t i;

    virCheckFlags(VIR_CONNECT_BASELINE_CPU_EXPAND_FEATURES |
                  VIR_CONNECT_BASELINE_CPU_MIGRATABLE, NULL);

    if (!(map = ppc64LoadMap()))
        goto error;

    if (!(model = ppc64ModelFind(map, cpus[0]->model))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown CPU model %s"), cpus[0]->model);
        goto error;
    }

    for (i = 0; i < ncpus; i++) {
        const struct ppc64_vendor *vnd;

        if (STRNEQ(cpus[i]->model, model->name)) {
            virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("CPUs are incompatible"));
            goto error;
        }

        if (!cpus[i]->vendor)
            continue;

        if (!(vnd = ppc64VendorFind(map, cpus[i]->vendor))) {
            virReportError(VIR_ERR_OPERATION_FAILED,
                           _("Unknown CPU vendor %s"), cpus[i]->vendor);
            goto error;
        }

        if (model->vendor) {
            if (model->vendor != vnd) {
                virReportError(VIR_ERR_OPERATION_FAILED,
                               _("CPU vendor %s of model %s differs from "
                                 "vendor %s"),
                               model->vendor->name, model->name,
                               vnd->name);
                goto error;
            }
        } else if (vendor) {
            if (vendor != vnd) {
                virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                               _("CPU vendors do not match"));
                goto error;
            }
        } else {
            vendor = vnd;
        }
    }

    if (VIR_ALLOC(cpu) < 0 ||
        VIR_STRDUP(cpu->model, model->name) < 0)
        goto error;

    if (vendor && VIR_STRDUP(cpu->vendor, vendor->name) < 0)
        goto error;

    cpu->type = VIR_CPU_TYPE_GUEST;
    cpu->match = VIR_CPU_MATCH_EXACT;

 cleanup:
    ppc64MapFree(map);

    return cpu;

 error:
    virCPUDefFree(cpu);
    cpu = NULL;
    goto cleanup;
}

static int
ppc64GetModels(char ***models)
{
    struct ppc64_map *map;
    struct ppc64_model *model;
    char *name;
    size_t nmodels = 0;

    if (!(map = ppc64LoadMap()))
        goto error;

    if (models && VIR_ALLOC_N(*models, 0) < 0)
        goto error;

    model = map->models;
    while (model != NULL) {
        if (models) {
            if (VIR_STRDUP(name, model->name) < 0)
                goto error;

            if (VIR_APPEND_ELEMENT(*models, nmodels, name) < 0)
                goto error;
        } else {
            nmodels++;
        }

        model = model->next;
    }

 cleanup:
    ppc64MapFree(map);

    return nmodels;

 error:
    if (models) {
        virStringFreeList(*models);
        *models = NULL;
    }
    nmodels = -1;
    goto cleanup;
}

struct cpuArchDriver cpuDriverPPC64 = {
    .name       = "ppc64",
    .arch       = archs,
    .narch      = ARRAY_CARDINALITY(archs),
    .compare    = ppc64Compare,
    .decode     = ppc64Decode,
    .encode     = NULL,
    .free       = ppc64DataFree,
    .nodeData   = ppc64NodeData,
    .guestData  = ppc64GuestData,
    .baseline   = ppc64Baseline,
    .update     = ppc64Update,
    .hasFeature = NULL,
    .getModels  = ppc64GetModels,
};
