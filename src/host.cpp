/*
 *  This file is part of the Xen Crashdump Analyser.
 *
 *  Foobar is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Foobar is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Copyright (c) 2011,2012 Citrix Inc.
 */

/**
 * @file src/host.cpp
 * @author Andrew Cooper
 */

#include "host.hpp"

#include "Xen.h"

#include "symbols.hpp"

#include "arch/x86_64/pagetable-walk.hpp"
#include "arch/x86_64/pcpu.hpp"
#include "arch/x86_64/domain.hpp"

#include "util/print-structures.hpp"
#include "util/log.hpp"
#include "memory.hpp"
#include "util/file.hpp"
#include "util/macros.hpp"

#include <new>
#include <sysexits.h>

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <string.h> // For strndup

Host::Host():
    once(false), arch(Elf::ELF_Unknown), nr_pcpus(0),
    pcpus(NULL), idle_vcpus(NULL),
    active_vcpus(),
    xen_major(0), xen_minor(0), xen_extra(NULL),
    xen_changeset(NULL), xen_compiler(NULL),
    xen_compile_date(NULL)
{}

Host::~Host()
{
    if ( this -> pcpus )
    {
        for ( int i = 0; i < this->nr_pcpus; ++i )
            SAFE_DELETE(this->pcpus[i]);
        delete [] this -> pcpus;
        this -> pcpus = NULL;
    }

    SAFE_DELETE_ARRAY(this->idle_vcpus);
    SAFE_DELETE_ARRAY(this->xen_extra);
    SAFE_DELETE_ARRAY(this->xen_changeset);
    SAFE_DELETE_ARRAY(this->xen_compiler);
    SAFE_DELETE_ARRAY(this->xen_compile_date);
}

bool Host::setup(const Elf * elf) throw ()
{
    if ( this -> once )
        return false;

    if ( elf->arch != Elf::ELF_64 )
    {
        LOG_ERROR("TODO - implement decoding for non-64bit Xen\n");
        return false;
    }

    this->arch = elf->arch;
    this->nr_pcpus = elf->nr_cpus;

    try
    {
        this->pcpus = new PCPU*[nr_pcpus];

        for ( int x = 0; x < nr_pcpus; ++x )
            if ( arch == Elf::ELF_64 )
                this->pcpus[x] = new x86_64PCPU();
            else
            {
                // Implement if necessary
            }

        this->idle_vcpus = new vaddr_t[nr_pcpus];

        for ( int x = 0; x < nr_pcpus; ++x )
            this->idle_vcpus[x] = -((vaddr_t)1);
    }
    catch ( const std::bad_alloc & )
    {
        LOG_ERROR("Bad alloc for PCPUs.  Kdump environment needs more memory\n");
        return false;
    }

    int pt_index = 0, xen_core_index = 0;
    for ( int x = 0; x < elf->nr_notes; ++x )
        switch ( elf->notes[x].type )
        {
        case NT_PRSTATUS:
            if ( ! this->pcpus[pt_index++]->parse_pr_status(
                     elf->notes[x].desc, elf->notes[x].desc_size) )
                return false;
            break;
        case XEN_ELFNOTE_CRASH_INFO:
            if ( ! this->parse_crash_xen_info(elf->notes[x].desc, elf->notes[x].desc_size) )
                return false;
            break;
        case XEN_ELFNOTE_CRASH_REGS:
            if ( ! this->pcpus[xen_core_index++]->parse_xen_crash_core(
                     elf->notes[x].desc, elf->notes[x].desc_size) )
                return false;
            break;
        default:
            break;
        }

    this->once = true;
    return true;
}

bool Host::parse_crash_xen_info(const char * buff, const size_t len) throw ()
{
    char * tmp = NULL;

    if ( arch != Elf::ELF_64 )
    {
        LOG_ERROR("TODO - implement decoding for non-64bit Xen\n");
        return false;
    }

    x86_64_crash_xen_info_t * info = (x86_64_crash_xen_info_t*)buff;

    if ( len != sizeof *info )
    {
        LOG_ERROR("Wrong size for crash_xen_info note.  Expected %zu, got %zu\n",
                  sizeof *info, len);
        return false;
    }

    try
    {
        this->xen_major = (int)info->xen_major_version;
        this->xen_minor = (int)info->xen_minor_version;

        tmp = new char[1024];
        tmp[0] = 0;

/// @cond
#define GET_STR(src, dst) do {                              \
            size_t sz = memory.read_str((src), tmp, 1023);  \
            (dst) = new char [ sz+1 ];                      \
            strcpy((dst), tmp); (dst)[sz]=0; } while (0)

        GET_STR(info->xen_extra_version, this->xen_extra);
        GET_STR(info->xen_changeset, this->xen_changeset);
        GET_STR(info->xen_compiler, this->xen_compiler);
        GET_STR(info->xen_compile_date, this->xen_compile_date);
#undef GET_STR
/// @endcond

    }
    catch ( const std::bad_alloc & )
    {
        LOG_ERROR("Bad alloc for PCPUs.  Kdump environment needs more memory\n");
    }
    CATCH_COMMON

    SAFE_DELETE_ARRAY(tmp);

    return true;
}


bool Host::decode_xen() throw ()
{
    const CPU & cpu = *static_cast<const CPU*>(this->pcpus[0]);

    LOG_INFO("Decoding physical CPU information.  %d PCPUs\n", this->nr_pcpus);

    if ( required_per_cpu_symbols != 0 )
    {
        LOG_ERROR("  Missing required per_cpu symbols. %#x\n",
                  required_per_cpu_symbols);
        return false;
    }

    try
    {
        if ( this->arch == Elf::ELF_64 )
        {
            LOG_DEBUG("  Reading idle vcpus\n");
            for ( int x = 0; x < this->nr_pcpus; ++x )
            {
                vaddr_t idle = idle_vcpu + (x * sizeof(uint64_t) );
                host.validate_xen_vaddr(idle);
                memory.read64_vaddr(cpu, idle, this->idle_vcpus[x]);
            }
        }
        else
        {
            // Implement if necessary
        }

        LOG_DEBUG("  Reading PCPUs vcpus\n");
        for (int x=0; x < nr_pcpus; ++x)
            if ( ! this->pcpus[x]->decode_extended_state() )
                LOG_ERROR("  Failed to decode extended state for pcpu %d\n", x);

        this->active_vcpus.reserve(nr_pcpus);
        LOG_DEBUG("  Generating active vcpu list\n");

        for (int x=0; x < nr_pcpus; ++x)
            switch(this->pcpus[x]->vcpu_state)
            {
            case PCPU::CTX_IDLE:
            case PCPU::CTX_RUNNING:
                this->active_vcpus.push_back(
                    vcpu_pair(this->pcpus[x]->vcpu->vcpu_ptr,
                              this->pcpus[x]->vcpu));
                break;
            case PCPU::CTX_SWITCH:
                this->active_vcpus.push_back(
                    vcpu_pair(this->pcpus[x]->ctx_from->vcpu_ptr,
                              this->pcpus[x]->ctx_from));
                break;
            default:
                break;
            }

        return true;
    }
    catch ( const std::bad_alloc & )
    {
        LOG_ERROR("Bad alloc for PCPUs.  Kdump environment needs more memory\n");
    }
    CATCH_COMMON

    return false;
}

int Host::print_xen(FILE * o) throw()
{
    int len = 0;
    const CPU & cpu = *static_cast<const CPU*>(this->pcpus[0]);
    char * cmdline = NULL;

    LOG_INFO("Writing Xen information to file\n");

    set_additional_log(o);

    try
    {
        // Print some header information for the host
        if ( this->xen_extra )
            len += fprintf(o, "Xen version:      %d.%d%s\n", this->xen_major,
                           this->xen_minor, this->xen_extra);
        if ( this->xen_changeset )
            len += fprintf(o, "Xen changeset:    %s\n", this->xen_changeset);
        if ( this->xen_compiler )
            len += fprintf(o, "Xen compiler:     %s\n", this->xen_compiler);
        if ( this->xen_compile_date )
            len += fprintf(o, "Xen compile date: %s\n", this->xen_compile_date);

        len += fprintf(o, "\n");

        // Try to find and print the saved command line string
        const Symbol * cmdline_sym = this->symtab.find("saved_cmdline");
        if ( ! cmdline_sym )
            len += fprintf(o, "Missing symbol for command line\n");
        else
        {
            try
            {
                // Size hardcoded in Xen
                cmdline = new char[1024];

                host.validate_xen_vaddr(cmdline_sym->address);
                memory.read_str_vaddr(cpu, cmdline_sym->address, cmdline, 1023);
                len += fprintf(o, "Xen command line: %s\n", cmdline);

                SAFE_DELETE_ARRAY(cmdline);
            }
            catch ( const std::bad_alloc & )
            {
                LOG_ERROR("Bad Alloc exception.  Out of memory\n");
            }
            CATCH_COMMON
        }

        len += fprintf(o, "\n");

        for (int x=0; x < nr_pcpus; ++x)
            len += this->pcpus[x]->print_state(o);

        len += fprintf(o, "\n  Console Ring:\n");

        if ( required_console_symbols == 0 )
        {
            uint64_t conring_ptr,length;
            uint32_t tmp;

            host.validate_xen_vaddr(conring);
            host.validate_xen_vaddr(conring_size);

            memory.read64_vaddr(cpu, conring, conring_ptr);
            memory.read32_vaddr(cpu, conring_size, tmp);
            length = tmp;

            if ( required_consolepc_symbols == 0 )
            {
                uint64_t prod,cons;

                host.validate_xen_vaddr(conringp);
                host.validate_xen_vaddr(conringc);

                memory.read32_vaddr(cpu, conringp, tmp);
                prod = tmp;
                memory.read32_vaddr(cpu, conringc, tmp);
                cons = tmp;

                len += print_console_ring(o, cpu, conring_ptr, length, prod, cons);
            }
            else
                len += print_console_ring(o, cpu, conring_ptr, length, 0, 0);
        }
        else
            len += fprintf(o, "    Missing conring symbols\n");
    }
    CATCH_COMMON

    SAFE_DELETE_ARRAY(cmdline);
    set_additional_log(NULL);
    return len;
}

int Host::print_domains() throw ()
{
    const CPU & cpuref = *static_cast<const CPU*>(this->pcpus[0]);
    int success = 0;

    LOG_INFO("Decoding Domains\n");

    if ( required_domain_symbols != 0 )
    {
        LOG_ERROR("Missing required domain symbols. %#x\n",
                  required_domain_symbols);
        return success;
    }

    if ( this->arch != Elf::ELF_64 )
    {
        // Implement if necessary
        LOG_ERROR("TODO - implement decoding for non-64bit Xen\n");
        return success;
    }

    Domain * dom = NULL;
    vaddr_t dom_ptr;
    FILE * fd = NULL;
    static char fname[32] = { 0 };

    try
    {
        host.validate_xen_vaddr(domain_list);
        memory.read64_vaddr(cpuref, domain_list, dom_ptr);
        LOG_DEBUG("  Domain pointer = 0x%016"PRIx64"\n", dom_ptr);

        while ( dom_ptr )
        {
            dom = new x86_64Domain();

            host.validate_xen_vaddr(dom_ptr);
            if ( ! dom->parse_basic(cpuref, dom_ptr) )
            {
                LOG_ERROR("  Failed to parse domain basics.  Cant continue\n");
                break;
            }

            /* Update dom_ptr as early as possible so we can continue around
             * this loop in the case of semi-recoverable failures.  The pointer
             * itself will be validated at the top of the next loop, so we get
             * a chance to print this information.
             */
            dom_ptr = dom->next_domain_ptr;
            LOG_INFO("  Found domain %"PRIu16"\n", dom->domain_id);

            snprintf(fname, sizeof fname, "dom%d.log", dom->domain_id);
            if ( ! (fd = fopen_in_outdir(fname, "w")) )
            {
                LOG_ERROR("    Failed to open file '%s' in output directory\n",
                          fname);
                goto loop_cont;
            }
            LOG_DEBUG("    Logging to '%s'\n", fname);

            /* As we have opened the file, might as well log errors to their
             * relevant context.
             */
            set_additional_log(fd);

            if ( ! dom->parse_vcpus_basic(cpuref) )
            {
                LOG_ERROR("    Failed to parse basic cpu information for domain %d\n",
                          dom->domain_id);
                goto loop_cont;
            }

            /* Try to match up this domains vcpus with vcpus running or idle on
             * Xen's pcpus.  If so, take the up-to-date register state.
             */
            for ( uint32_t v = 0; v < dom->max_cpus; v++ )
            {
                unsigned int p; bool found;

                if ( ! dom->vcpus[v]->is_up() )
                {
                    LOG_DEBUG("    Dom%"PRIu16" vcpu%"PRIu32" was not up\n", dom->domain_id, v);
                    continue;
                }

                for (p = 0, found = false; p < this->active_vcpus.size(); p++)
                    if ( this->active_vcpus[p].first == dom->vcpus[v]->vcpu_ptr )
                    {
                        found = true;
                        break;
                    }

                if ( found )
                {
                    LOG_DEBUG("    Dom%"PRIu16" vcpu%"PRIu32" was active on pcpu%u\n",
                              dom->domain_id, v, p);
                    dom->vcpus[v]->parse_regs_from_active(this->active_vcpus[p].second);
                }
                else
                {
                    LOG_DEBUG("    Dom%"PRIu16" vcpu%"PRIu32" was not active\n",
                              dom->domain_id, v);
                    dom->vcpus[v]->parse_regs_from_struct();
                    dom->vcpus[v]->runstate = VCPU::RST_NONE;
                }
            }

            dom->print_state(fd);
            ++success;

        loop_cont:
            set_additional_log(NULL);
            if ( fd )  { fclose(fd); fd = NULL;  }
            SAFE_DELETE(dom);
        }
    }
    catch ( const std::bad_alloc & )
    {
        LOG_ERROR("Bad Alloc exception.  Out of memory\n");
    }
    CATCH_COMMON

    set_additional_log(NULL);
    if ( fd )  { fclose(fd); fd = NULL;  }
    SAFE_DELETE(dom);

    return success;
}

bool Host::validate_xen_vaddr(const vaddr_t & vaddr, const bool except)
{
    switch(this->arch)
    {
        // Values from xen/include/asm-x86/config.h
    case Elf::ELF_64:
        // Xen .text, .bss etc (1GB)
        if ( 0xffff82c480000000ULL <= vaddr && vaddr <= 0xffff82c4bfffffffULL )
            return true;
        // 1:1 direct mapping of physical memory (120TB)
        else if ( 0xffff830000000000ULL <= vaddr && vaddr <= 0xffff87ffffffffffULL )
            return true;
    default:
        break;
    }

    if ( except )
        throw validate(vaddr);
    return false;
}

/// Host container
Host host;

/*
 * Local variables:
 * mode: C++
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */