/*******************************************************************************
    Copyright (c) 2018 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*******************************************************************************/

#include "uvm_tools.h"
#include "uvm_va_range.h"
#include "uvm_ats_ibm.h"
#include "uvm_ats_faults.h"

NV_STATUS uvm_ats_service_fault_entry(uvm_gpu_va_space_t *gpu_va_space,
                                      uvm_fault_buffer_entry_t *current_entry,
                                      uvm_ats_fault_invalidate_t *ats_invalidate)
{
    NvU64 gmmu_region_base;
    bool in_gmmu_region;
    NV_STATUS status = NV_OK;
    uvm_fault_access_type_t service_access_type;

    UVM_ASSERT(g_uvm_global.ats.enabled);
    UVM_ASSERT(gpu_va_space->ats.enabled);
    UVM_ASSERT(uvm_gpu_va_space_state(gpu_va_space) == UVM_GPU_VA_SPACE_STATE_ACTIVE);

    UVM_ASSERT(current_entry->fault_access_type ==
               uvm_fault_access_type_mask_highest(current_entry->access_type_mask));

    service_access_type = current_entry->fault_access_type;

    // ATS lookups are disabled on all addresses within the same
    // UVM_GMMU_ATS_GRANULARITY as existing GMMU mappings (see documentation in
    // uvm_mmu.h). User mode is supposed to reserve VAs as appropriate to
    // prevent any system memory allocations from falling within the NO_ATS
    // range of other GMMU mappings, so this shouldn't happen during normal
    // operation. However, since this scenario may lead to infinite fault loops,
    // we handle it by canceling the fault.
    //
    // TODO: Bug 2103669: Remove redundant VA range lookups
    gmmu_region_base = UVM_ALIGN_DOWN(current_entry->fault_address, UVM_GMMU_ATS_GRANULARITY);
    in_gmmu_region = !uvm_va_space_range_empty(current_entry->va_space,
                                               gmmu_region_base,
                                               gmmu_region_base + UVM_GMMU_ATS_GRANULARITY - 1);
    if (in_gmmu_region) {
        status = NV_ERR_INVALID_ADDRESS;
    }
    else {
        // TODO: Bug 2103669: Service more than a single fault at a time
        status = uvm_ats_ibm_service_fault(gpu_va_space, current_entry->fault_address, service_access_type);
    }

    // Do not flag prefetch faults as fatal unless something fatal happened
    if (status == NV_ERR_INVALID_ADDRESS) {
        if (current_entry->fault_access_type != UVM_FAULT_ACCESS_TYPE_PREFETCH) {
            current_entry->is_fatal = true;
            current_entry->fatal_reason = uvm_tools_status_to_fatal_fault_reason(status);

            // Compute cancel mode for replayable faults
            if (current_entry->is_replayable) {
                if (service_access_type == UVM_FAULT_ACCESS_TYPE_READ || in_gmmu_region)
                    current_entry->replayable.cancel_va_mode = UVM_FAULT_CANCEL_VA_MODE_ALL;
                else
                    current_entry->replayable.cancel_va_mode = UVM_FAULT_CANCEL_VA_MODE_WRITE_AND_ATOMIC;

                // If there are pending read accesses on the same page, we have to
                // service them before we can cancel the write/atomic faults. So we
                // retry with read fault access type.
                if (!in_gmmu_region &&
                    current_entry->fault_access_type > UVM_FAULT_ACCESS_TYPE_READ &&
                    uvm_fault_access_type_mask_test(current_entry->access_type_mask, UVM_FAULT_ACCESS_TYPE_READ)) {
                    status = uvm_ats_ibm_service_fault(gpu_va_space,
                                                       current_entry->fault_address,
                                                       UVM_FAULT_ACCESS_TYPE_READ);

                    // If read accesses are also invalid, cancel the fault. If a
                    // different error code is returned, exit
                    if (status == NV_ERR_INVALID_ADDRESS)
                        current_entry->replayable.cancel_va_mode = UVM_FAULT_CANCEL_VA_MODE_ALL;
                    else if (status != NV_OK)
                        return status;
                }
            }
        }
        else {
            current_entry->is_invalid_prefetch = true;
        }

        // Do not fail overall fault servicing due to logical errors
        status = NV_OK;
    }

    // The Linux kernel never invalidates TLB entries on mapping permission
    // upgrade. This is a problem if the GPU has cached entries with the old
    // permission. The GPU will re-fetch the entry if the PTE is invalid and
    // page size is not 4K (this is the case on P9). However, if a page gets
    // upgraded from R/O to R/W and GPU has the PTEs cached with R/O
    // permissions we will enter an infinite loop because we just forward the
    // fault to the Linux kernel and it will see that the permissions in the
    // page table are correct. Therefore, we flush TLB entries on ATS write
    // faults.
    if (!current_entry->is_fatal && current_entry->fault_access_type > UVM_FAULT_ACCESS_TYPE_READ) {
        if (!ats_invalidate->write_faults_in_batch) {
            uvm_tlb_batch_begin(&gpu_va_space->page_tables, &ats_invalidate->write_faults_tlb_batch);
            ats_invalidate->write_faults_in_batch = true;
        }

        uvm_tlb_batch_invalidate(&ats_invalidate->write_faults_tlb_batch,
                                 current_entry->fault_address,
                                 PAGE_SIZE,
                                 PAGE_SIZE,
                                 UVM_MEMBAR_NONE);
    }

    return status;
}

NV_STATUS uvm_ats_invalidate_tlbs(uvm_gpu_va_space_t *gpu_va_space,
                                  uvm_ats_fault_invalidate_t *ats_invalidate,
                                  uvm_tracker_t *out_tracker)
{
    NV_STATUS status;
    uvm_push_t push;

    if (!ats_invalidate->write_faults_in_batch)
        return NV_OK;

    UVM_ASSERT(gpu_va_space);
    UVM_ASSERT(gpu_va_space->ats.enabled);

    status = uvm_push_begin(gpu_va_space->gpu->channel_manager,
                            UVM_CHANNEL_TYPE_MEMOPS,
                            &push,
                            "Invalidate ATS entries");

    if (status == NV_OK) {
        uvm_tlb_batch_end(&ats_invalidate->write_faults_tlb_batch, &push, UVM_MEMBAR_NONE);
        uvm_push_end(&push);

        // Add this push to the GPU's tracker so that fault replays/clears can
        // wait on it
        status = uvm_tracker_add_push_safe(out_tracker, &push);
    }

    ats_invalidate->write_faults_in_batch = false;

    return status;
}
