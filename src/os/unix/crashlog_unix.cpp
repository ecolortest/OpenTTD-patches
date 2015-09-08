/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file crashlog_unix.cpp Unix crash log handler */

#include "../../stdafx.h"
#include "../../crashlog.h"
#include "../../string_func.h"
#include "../../gamelog.h"
#include "../../saveload/saveload.h"

#include <errno.h>
#include <signal.h>
#include <sys/utsname.h>

#if defined(__GLIBC__)
/* Execinfo (and thus making stacktraces) is a GNU extension */
#	include <execinfo.h>
#if defined(WITH_DL)
#   include <dlfcn.h>
#endif
#if defined(WITH_DEMANGLE)
#   include <cxxabi.h>
#endif
#if defined(WITH_BFD)
#   include <bfd.h>
#endif
#elif defined(SUNOS)
#	include <ucontext.h>
#	include <dlfcn.h>
#endif

#if defined(__NetBSD__)
#include <unistd.h>
#endif

#include "../../safeguards.h"

#if defined(WITH_BFD)
struct line_info {
	bfd_vma addr;
	bfd *abfd;
	asymbol **syms;
	long sym_count;
	const char *file_name;
	const char *function_name;
	bfd_vma function_addr;
	unsigned int line;
	bool found;

	line_info(bfd_vma addr_) : addr(addr_), abfd(NULL), syms(NULL), sym_count(0),
			file_name(NULL), function_name(NULL), function_addr(0), line(0), found(false) {}

	~line_info()
	{
		free(syms);
		if (abfd != NULL) bfd_close(abfd);
	}
};

static void find_address_in_section(bfd *abfd, asection *section, void *data)
{
	line_info *info = static_cast<line_info *>(data);
	if (info->found) return;

	if ((bfd_get_section_flags(abfd, section) & SEC_ALLOC) == 0) return;

	bfd_vma vma = bfd_get_section_vma(abfd, section);
	if (info->addr < vma) return;

	bfd_size_type size = bfd_section_size(abfd, section);
	if (info->addr >= vma + size) return;

	info->found = bfd_find_nearest_line(abfd, section, info->syms, info->addr - vma,
			&(info->file_name), &(info->function_name), &(info->line));

	if (info->found) {
		for (long i = 0; i < info->sym_count; i++) {
			asymbol *sym = info->syms[i];
			if (sym->flags & (BSF_LOCAL | BSF_GLOBAL) && strcmp(sym->name, info->function_name) == 0) {
				info->function_addr = sym->value + vma;
			}
		}
	}
}

void lookup_addr_bfd(const char *obj_file_name, line_info &info)
{
	info.abfd = bfd_openr(obj_file_name, NULL);

	if (info.abfd == NULL) return;

	if (!bfd_check_format(info.abfd, bfd_object) || (bfd_get_file_flags(info.abfd) & HAS_SYMS) == 0) return;

	unsigned int size;
	info.sym_count = bfd_read_minisymbols(info.abfd, false, (void**) &(info.syms), &size);
	if (info.sym_count <= 0) {
		info.sym_count = bfd_read_minisymbols(info.abfd, true, (void**) &(info.syms), &size);
	}
	if (info.sym_count <= 0) return;

	bfd_map_over_sections(info.abfd, find_address_in_section, &info);
}
#endif

/**
 * Unix implementation for the crash logger.
 */
class CrashLogUnix : public CrashLog {
	/** Signal that has been thrown. */
	int signum;

	/* virtual */ char *LogOSVersion(char *buffer, const char *last) const
	{
		struct utsname name;
		if (uname(&name) < 0) {
			return buffer + seprintf(buffer, last, "Could not get OS version: %s\n", strerror(errno));
		}

		return buffer + seprintf(buffer, last,
				"Operating system:\n"
				" Name:     %s\n"
				" Release:  %s\n"
				" Version:  %s\n"
				" Machine:  %s\n",
				name.sysname,
				name.release,
				name.version,
				name.machine
		);
	}

	/* virtual */ char *LogError(char *buffer, const char *last, const char *message) const
	{
		return buffer + seprintf(buffer, last,
				"Crash reason:\n"
				" Signal:  %s (%d)\n"
				" Message: %s\n\n",
				strsignal(this->signum),
				this->signum,
				message == NULL ? "<none>" : message
		);
	}

#if defined(SUNOS)
	/** Data needed while walking up the stack */
	struct StackWalkerParams {
		char **bufptr;    ///< Buffer
		const char *last; ///< End of buffer
		int counter;      ///< We are at counter-th stack level
	};

	/**
	 * Callback used while walking up the stack.
	 * @param pc program counter
	 * @param sig 'active' signal (unused)
	 * @param params parameters
	 * @return always 0, continue walking up the stack
	 */
	static int SunOSStackWalker(uintptr_t pc, int sig, void *params)
	{
		StackWalkerParams *wp = (StackWalkerParams *)params;

		/* Resolve program counter to file and nearest symbol (if possible) */
		Dl_info dli;
		if (dladdr((void *)pc, &dli) != 0) {
			*wp->bufptr += seprintf(*wp->bufptr, wp->last, " [%02i] %s(%s+0x%x) [0x%x]\n",
					wp->counter, dli.dli_fname, dli.dli_sname, (int)((byte *)pc - (byte *)dli.dli_saddr), (uint)pc);
		} else {
			*wp->bufptr += seprintf(*wp->bufptr, wp->last, " [%02i] [0x%x]\n", wp->counter, (uint)pc);
		}
		wp->counter++;

		return 0;
	}
#endif

	/* virtual */ char *LogStacktrace(char *buffer, const char *last) const
	{
		buffer += seprintf(buffer, last, "Stacktrace:\n");
#if defined(__GLIBC__)
#if defined(WITH_BFD)
		bfd_init();
#endif
		void *trace[64];
		int trace_size = backtrace(trace, lengthof(trace));

		char **messages = backtrace_symbols(trace, trace_size);
		for (int i = 0; i < trace_size; i++) {
#if defined(WITH_DL)
			Dl_info info;
			int dladdr_result = dladdr(trace[i], &info);
			const char *func_name = info.dli_sname;
			void *func_addr = info.dli_saddr;
			const char *file_name = NULL;
			unsigned int line_num = 0;
#if defined(WITH_BFD)
			/* subtract one to get the line before the return address, i.e. the function call line */
			line_info bfd_info(reinterpret_cast<bfd_vma>(trace[i]) - 1);
			if (dladdr_result && info.dli_fname) {
				lookup_addr_bfd(info.dli_fname, bfd_info);
				if (bfd_info.file_name != NULL) file_name = bfd_info.file_name;
				if (bfd_info.function_name != NULL) func_name = bfd_info.function_name;
				if (bfd_info.function_addr != 0) func_addr = reinterpret_cast<void *>(bfd_info.function_addr);
				line_num = bfd_info.line;
			}
#endif
			bool ok = true;
			const int ptr_str_size = (2 + sizeof(void*) * 2);
			if (dladdr_result && func_name) {
				int status = -1;
				char *demangled = NULL;
#if defined(WITH_DEMANGLE)
				demangled = abi::__cxa_demangle(func_name, NULL, 0, &status);
#endif
				const char *name = (demangled != NULL && status == 0) ? demangled : func_name;
				buffer += seprintf(buffer, last, " [%02i] %*p %-40s %s + 0x%zx\n", i, ptr_str_size,
						trace[i], info.dli_fname, name, (char *)trace[i] - (char *)func_addr);
				free(demangled);
			} else if (dladdr_result && info.dli_fname) {
				buffer += seprintf(buffer, last, " [%02i] %*p %-40s + 0x%zx\n", i, ptr_str_size,
						trace[i], info.dli_fname, (char *)trace[i] - (char *)info.dli_fbase);
			} else {
				ok = false;
			}
			if (file_name != NULL) {
				buffer += seprintf(buffer, last, "%*s%s:%u\n", 7 + ptr_str_size, "", file_name, line_num);
			}
			if (ok) continue;
#endif
			buffer += seprintf(buffer, last, " [%02i] %s\n", i, messages[i]);
		}
		free(messages);
#elif defined(SUNOS)
		ucontext_t uc;
		if (getcontext(&uc) != 0) {
			buffer += seprintf(buffer, last, " getcontext() failed\n\n");
			return buffer;
		}

		StackWalkerParams wp = { &buffer, last, 0 };
		walkcontext(&uc, &CrashLogUnix::SunOSStackWalker, &wp);
#else
		buffer += seprintf(buffer, last, " Not supported.\n");
#endif
		return buffer + seprintf(buffer, last, "\n");
	}
public:
	/**
	 * A crash log is always generated by signal.
	 * @param signum the signal that was caused by the crash.
	 */
	CrashLogUnix(int signum) :
		signum(signum)
	{
	}
};

/** The signals we want our crash handler to handle. */
static const int _signals_to_handle[] = { SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL };

/**
 * Entry point for the crash handler.
 * @note Not static so it shows up in the backtrace.
 * @param signum the signal that caused us to crash.
 */
static void CDECL HandleCrash(int signum)
{
	/* Disable all handling of signals by us, so we don't go into infinite loops. */
	for (const int *i = _signals_to_handle; i != endof(_signals_to_handle); i++) {
		signal(*i, SIG_DFL);
	}

	if (GamelogTestEmergency()) {
		printf("A serious fault condition occurred in the game. The game will shut down.\n");
		printf("As you loaded an emergency savegame no crash information will be generated.\n");
		abort();
	}

	if (SaveloadCrashWithMissingNewGRFs()) {
		printf("A serious fault condition occurred in the game. The game will shut down.\n");
		printf("As you loaded an savegame for which you do not have the required NewGRFs\n");
		printf("no crash information will be generated.\n");
		abort();
	}

	CrashLogUnix log(signum);
	log.MakeCrashLog();

	CrashLog::AfterCrashLogCleanup();
	abort();
}

/* static */ void CrashLog::InitialiseCrashLog()
{
	for (const int *i = _signals_to_handle; i != endof(_signals_to_handle); i++) {
		signal(*i, HandleCrash);
	}
}
