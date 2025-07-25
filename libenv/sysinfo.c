/*
  Copyright 2024 Northern.tech AS

  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; version 3.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <platform.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <sysinfo.h>
#include <sysinfo_priv.h>
#include <cf3.extern.h>
#include <eval_context.h>
#include <files_names.h>
#include <files_interfaces.h>
#include <hash.h>
#include <scope.h>
#include <item_lib.h>
#include <matching.h>
#include <systype.h>
#include <unix.h>
#include <string_lib.h>
#include <regex.h>                                       /* StringMatchFull */
#include <misc_lib.h>
#include <file_lib.h>
#include <rlist.h>
#include <audit.h>
#include <pipes.h>
#include <known_dirs.h>
#include <files_lib.h>
#include <printsize.h>
#include <cf-windows-functions.h>
#include <ornaments.h>
#include <feature.h>
#include <evalfunction.h>
#include <json-utils.h>
#include <unix.h>               /* GetCurrentUserName() */
#include <glob_lib.h>

#ifdef HAVE_ZONE_H
# include <zone.h>
#endif

// HP-UX mpctl() for $(sys.cpus) on HP-UX - Mantis #1069
#ifdef HAVE_SYS_MPCTL_H
# include <sys/mpctl.h>
#endif

/* Linux.
   WARNING keep this before the #include <sys/sysctl.h> because of glibc bug:
   https://sourceware.org/bugzilla/show_bug.cgi?id=140 */
#ifdef HAVE_STRUCT_SYSINFO_UPTIME
# include <sys/sysinfo.h>
#endif

/* BSD, MAC OS X uptime calculation use KERN_BOOTTIME sysctl. */
#ifdef HAVE_SYS_SYSCTL_H
# ifdef HAVE_SYS_PARAM_H
#  include <sys/param.h>
# endif
# ifdef __linux__
#  include <linux/sysctl.h>
# else
# include <sys/sysctl.h>
# endif
#endif


/*****************************************************/
// Uptime calculation settings for GetUptimeSeconds() - Mantis #1134

/* Listed here in priority order, i.e. first come the platform-specific
 * ways. If nothing works, one of the last, most generic ways should. */

#ifndef __MINGW32__                 /* Windows is implemented in Enterprise */

// HP-UX: pstat_getproc(2) on init (pid 1)
#if defined(__hpux)
# include <sys/param.h>
# include <sys/pstat.h>
# define BOOT_TIME_WITH_PSTAT_GETPROC

// Solaris: kstat() for kernel statistics
// See http://dsc.sun.com/solaris/articles/kstatc.html
// BSD also has a kstat.h (albeit in sys), so check __sun just to be paranoid

/**
 * @WARNING: Commented out because inside a Solaris 10 zone this gives the
 *           uptime of the host machine (the hypervisor). We thus choose to
 *           use UTMP for Solaris.
 */
/*
#elif defined(__sun) && defined(HAVE_KSTAT_H)
# include <kstat.h>
# define BOOT_TIME_WITH_KSTAT
*/

// BSD: sysctl(3) to get kern.boottime, CPU count, etc.
// See http://www.unix.com/man-page/FreeBSD/3/sysctl/
// Linux also has sys/sysctl.h, so we check KERN_BOOTTIME to make sure it's BSD
#elif defined(HAVE_SYS_SYSCTL_H) && defined(KERN_BOOTTIME)
# define BOOT_TIME_WITH_SYSCTL

// GNU/Linux: struct sysinfo.uptime
#elif defined(HAVE_STRUCT_SYSINFO_UPTIME)
# define BOOT_TIME_WITH_SYSINFO

/* Generic System V way, available in most platforms. */
#elif defined(HAVE_UTMP_H)
# include <utmp.h>
# define BOOT_TIME_WITH_UTMP

/* POSIX alternative (utmp.h does not exist on BSDs). */
#elif defined(HAVE_UTMPX_H)
# include <utmpx.h>
# define BOOT_TIME_WITH_UTMPX

#else
// Most generic way: {stat("/proc/1")}.st_ctime
// TODO in Solaris zones init is not guaranteed to be PID 1!
#define BOOT_TIME_WITH_PROCFS

#endif

/* Fallback uptime calculation: Parse the "uptime" command in case the
 * platform-specific way fails or returns absurd number. */
static time_t GetBootTimeFromUptimeCommand(time_t now);

#endif  /* ifndef __MINGW32__ */

#define LSB_RELEASE_FILENAME "/etc/lsb-release"
#define DEBIAN_VERSION_FILENAME "/etc/debian_version"
#define DEBIAN_ISSUE_FILENAME "/etc/issue"


/*****************************************************/

static char *FindNextInteger(char *str, char **num);

void CalculateDomainName(const char *nodename, const char *dnsname,
                         char *fqname, size_t fqname_size,
                         char *uqname, size_t uqname_size,
                         char *domain, size_t domain_size);

#ifdef __APPLE__
static void Apple_Version(EvalContext *ctx);
#endif

#ifdef __linux__
static int Linux_Fedora_Version(EvalContext *ctx);
static int Linux_Redhat_Version(EvalContext *ctx);
static void Linux_Amazon_Version(EvalContext *ctx);
static void Linux_Alpine_Version(EvalContext *ctx);
static void Linux_Oracle_VM_Server_Version(EvalContext *ctx);
static void Linux_Oracle_Version(EvalContext *ctx);
static int Linux_Suse_Version(EvalContext *ctx);
static int Linux_Slackware_Version(EvalContext *ctx, char *filename);
static int Linux_Debian_Version(EvalContext *ctx);
static int Linux_Misc_Version(EvalContext *ctx);
static int Linux_Mandrake_Version(EvalContext *ctx);
static int Linux_Mandriva_Version(EvalContext *ctx);
static int Linux_Mandriva_Version_Real(EvalContext *ctx, char *filename, char *relstring, char *vendor);
static int VM_Version(EvalContext *ctx);
static int Xen_Domain(EvalContext *ctx);
static int EOS_Version(EvalContext *ctx);
static int MiscOS(EvalContext *ctx);
static void OpenVZ_Detect(EvalContext *ctx);

static bool ReadLine(const char *filename, char *buf, int bufsize);
static FILE *ReadFirstLine(const char *filename, char *buf, int bufsize);
#endif

#ifdef XEN_CPUID_SUPPORT
static void Xen_Cpuid(uint32_t idx, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
static bool Xen_Hv_Check(void);
#endif

static void GetCPUInfo(EvalContext *ctx);

static const char *const CLASSATTRIBUTES[][3] =
{
    [PLATFORM_CONTEXT_UNKNOWN] = {"-", "-", "-"},       /* as appear here are matched. The fields are sysname and machine */
    [PLATFORM_CONTEXT_OPENVZ] = {"virt_host_vz_vzps", ".*", ".*"}, /* VZ with vzps */
    [PLATFORM_CONTEXT_HP] = {"hp-ux", ".*", ".*"},      /* hpux */
    [PLATFORM_CONTEXT_AIX] = {"aix", ".*", ".*"},       /* aix */
    [PLATFORM_CONTEXT_LINUX] = {"linux", ".*", ".*"},   /* linux */
    [PLATFORM_CONTEXT_BUSYBOX] = {"busybox", ".*", ".*"}, /* linux w/ busybox - warning uname returns linux */
    [PLATFORM_CONTEXT_SOLARIS] = {"sunos", ".*",
                                  "5\\.1[1-9].*"},      /* new solaris, SunOS >= 5.11 */
    [PLATFORM_CONTEXT_SUN_SOLARIS] = {"sunos", ".*",
                                      "5\\.([2-9]|10)(\\..*)?"}, /* old solaris, SunOS < 5.11 */
    [PLATFORM_CONTEXT_FREEBSD] = {"freebsd", ".*", ".*"}, /* freebsd */
    [PLATFORM_CONTEXT_NETBSD] = {"netbsd", ".*", ".*"},   /* NetBSD */
    [PLATFORM_CONTEXT_CRAYOS] = {"sn.*", "cray*", ".*"},  /* cray */
    [PLATFORM_CONTEXT_WINDOWS_NT] = {"cygwin_nt.*", ".*", ".*"}, /* NT (cygwin) */
    [PLATFORM_CONTEXT_SYSTEMV] = {"unix_sv", ".*", ".*"}, /* Unixware */
    [PLATFORM_CONTEXT_OPENBSD] = {"openbsd", ".*", ".*"}, /* OpenBSD */
    [PLATFORM_CONTEXT_CFSCO] = {"sco_sv", ".*", ".*"},    /* SCO */
    [PLATFORM_CONTEXT_DARWIN] = {"darwin", ".*", ".*"},   /* Darwin, aka MacOS X */
    [PLATFORM_CONTEXT_QNX] = {"qnx", ".*", ".*"},         /* qnx  */
    [PLATFORM_CONTEXT_DRAGONFLY] = {"dragonfly", ".*", ".*"}, /* dragonfly */
    [PLATFORM_CONTEXT_MINGW] = {"windows_nt.*", ".*", ".*"},  /* NT (native) */
    [PLATFORM_CONTEXT_VMWARE] = {"vmkernel", ".*", ".*"}, /* VMWARE / ESX */
    [PLATFORM_CONTEXT_ANDROID] = {"android", ".*", ".*"}, /* android: Warning uname returns linux */
};

static const char *const VRESOLVCONF[] =
{
    [PLATFORM_CONTEXT_UNKNOWN] = "-",
    [PLATFORM_CONTEXT_OPENVZ] = "/etc/resolv.conf",      /* virt_host_vz_vzps */
    [PLATFORM_CONTEXT_HP] = "/etc/resolv.conf",          /* hpux */
    [PLATFORM_CONTEXT_AIX] = "/etc/resolv.conf",         /* aix */
    [PLATFORM_CONTEXT_LINUX] = "/etc/resolv.conf",       /* linux */
    [PLATFORM_CONTEXT_BUSYBOX] = "/etc/resolv.conf",     /* linux */
    [PLATFORM_CONTEXT_SOLARIS] = "/etc/resolv.conf",     /* new solaris */
    [PLATFORM_CONTEXT_SUN_SOLARIS] = "/etc/resolv.conf", /* old solaris */
    [PLATFORM_CONTEXT_FREEBSD] = "/etc/resolv.conf",     /* freebsd */
    [PLATFORM_CONTEXT_NETBSD] = "/etc/resolv.conf",      /* netbsd */
    [PLATFORM_CONTEXT_CRAYOS] = "/etc/resolv.conf",      /* cray */
    [PLATFORM_CONTEXT_WINDOWS_NT] = "/etc/resolv.conf",  /* NT */
    [PLATFORM_CONTEXT_SYSTEMV] = "/etc/resolv.conf",     /* Unixware */
    [PLATFORM_CONTEXT_OPENBSD] = "/etc/resolv.conf",     /* openbsd */
    [PLATFORM_CONTEXT_CFSCO] = "/etc/resolv.conf",       /* sco */
    [PLATFORM_CONTEXT_DARWIN] = "/etc/resolv.conf",      /* darwin */
    [PLATFORM_CONTEXT_QNX] = "/etc/resolv.conf",         /* qnx */
    [PLATFORM_CONTEXT_DRAGONFLY] = "/etc/resolv.conf",   /* dragonfly */
    [PLATFORM_CONTEXT_MINGW] = "",                       /* mingw */
    [PLATFORM_CONTEXT_VMWARE] = "/etc/resolv.conf",      /* vmware */
    [PLATFORM_CONTEXT_ANDROID] = "",                     /* android */
};

static const char *const VMAILDIR[] =
{
    [PLATFORM_CONTEXT_UNKNOWN] = "-",
    [PLATFORM_CONTEXT_OPENVZ] = "/var/spool/mail", /* virt_host_vz_vzps */
    [PLATFORM_CONTEXT_HP] = "/var/mail",           /* hpux */
    [PLATFORM_CONTEXT_AIX] = "/var/spool/mail",    /* aix */
    [PLATFORM_CONTEXT_LINUX] = "/var/spool/mail",  /* linux */
    [PLATFORM_CONTEXT_BUSYBOX] = "",               /* linux */
    [PLATFORM_CONTEXT_SOLARIS] = "/var/mail",      /* new solaris */
    [PLATFORM_CONTEXT_SUN_SOLARIS] = "/var/mail",  /* old solaris */
    [PLATFORM_CONTEXT_FREEBSD] = "/var/mail",      /* freebsd */
    [PLATFORM_CONTEXT_NETBSD] = "/var/mail",       /* netbsd */
    [PLATFORM_CONTEXT_CRAYOS] = "/usr/mail",       /* cray */
    [PLATFORM_CONTEXT_WINDOWS_NT] = "N/A",         /* NT */
    [PLATFORM_CONTEXT_SYSTEMV] = "/var/mail",      /* Unixware */
    [PLATFORM_CONTEXT_OPENBSD] = "/var/mail",      /* openbsd */
    [PLATFORM_CONTEXT_CFSCO] = "/var/spool/mail",  /* sco */
    [PLATFORM_CONTEXT_DARWIN] = "/var/mail",       /* darwin */
    [PLATFORM_CONTEXT_QNX] = "/var/spool/mail",    /* qnx */
    [PLATFORM_CONTEXT_DRAGONFLY] = "/var/mail",    /* dragonfly */
    [PLATFORM_CONTEXT_MINGW] = "",                 /* mingw */
    [PLATFORM_CONTEXT_VMWARE] = "/var/spool/mail", /* vmware */
    [PLATFORM_CONTEXT_ANDROID] = "",               /* android */
};

static const char *const VEXPORTS[] =
{
    [PLATFORM_CONTEXT_UNKNOWN] = "-",
    [PLATFORM_CONTEXT_OPENVZ] = "/etc/exports",         /* virt_host_vz_vzps */
    [PLATFORM_CONTEXT_HP] = "/etc/exports",             /* hpux */
    [PLATFORM_CONTEXT_AIX] = "/etc/exports",            /* aix */
    [PLATFORM_CONTEXT_LINUX] = "/etc/exports",          /* linux */
    [PLATFORM_CONTEXT_BUSYBOX] = "",                    /* linux */
    [PLATFORM_CONTEXT_SOLARIS] = "/etc/dfs/dfstab",     /* new solaris */
    [PLATFORM_CONTEXT_SUN_SOLARIS] = "/etc/dfs/dfstab", /* old solaris */
    [PLATFORM_CONTEXT_FREEBSD] = "/etc/exports",        /* freebsd */
    [PLATFORM_CONTEXT_NETBSD] = "/etc/exports",         /* netbsd */
    [PLATFORM_CONTEXT_CRAYOS] = "/etc/exports",         /* cray */
    [PLATFORM_CONTEXT_WINDOWS_NT] = "/etc/exports",     /* NT */
    [PLATFORM_CONTEXT_SYSTEMV] = "/etc/dfs/dfstab",     /* Unixware */
    [PLATFORM_CONTEXT_OPENBSD] = "/etc/exports",        /* openbsd */
    [PLATFORM_CONTEXT_CFSCO] = "/etc/dfs/dfstab",       /* sco */
    [PLATFORM_CONTEXT_DARWIN] = "/etc/exports",         /* darwin */
    [PLATFORM_CONTEXT_QNX] = "/etc/exports",            /* qnx */
    [PLATFORM_CONTEXT_DRAGONFLY] = "/etc/exports",      /* dragonfly */
    [PLATFORM_CONTEXT_MINGW] = "",                      /* mingw */
    [PLATFORM_CONTEXT_VMWARE] = "none",                 /* vmware */
    [PLATFORM_CONTEXT_ANDROID] = ""  ,                  /* android */
};


/*******************************************************************/

void CalculateDomainName(const char *nodename, const char *dnsname,
                         char *fqname, size_t fqname_size,
                         char *uqname, size_t uqname_size,
                         char *domain, size_t domain_size)
{
    if (strstr(dnsname, "."))
    {
        strlcpy(fqname, dnsname, fqname_size);
    }
    else
    {
        strlcpy(fqname, nodename, fqname_size);
    }

    if ((strncmp(nodename, fqname, strlen(nodename)) == 0) && (fqname[strlen(nodename)] == '.'))
    {
        /* If hostname is not qualified */
        strlcpy(domain, fqname + strlen(nodename) + 1, domain_size);
        strlcpy(uqname, nodename, uqname_size);
    }
    else
    {
        /* If hostname is qualified */

        char *p = strchr(nodename, '.');

        if (p != NULL)
        {
            strlcpy(uqname, nodename, MIN(uqname_size, p - nodename + 1));
            strlcpy(domain, p + 1, domain_size);
        }
        else
        {
            strlcpy(uqname, nodename, uqname_size);
            strlcpy(domain, "", domain_size);
        }
    }
}

/*******************************************************************/

void DetectDomainName(EvalContext *ctx, const char *orig_nodename)
{
    char nodename[CF_BUFSIZE];

    strlcpy(nodename, orig_nodename, sizeof(nodename));
    ToLowerStrInplace(nodename);

    char dnsname[CF_BUFSIZE] = "";
    char fqn[CF_BUFSIZE];

    if (gethostname(fqn, sizeof(fqn)) != -1)
    {
        struct hostent *hp;

        if ((hp = gethostbyname(fqn)))
        {
            strlcpy(dnsname, hp->h_name, sizeof(dnsname));
            ToLowerStrInplace(dnsname);
        }
    }

    nt_static_assert(sizeof(VFQNAME) > 255);
    nt_static_assert(sizeof(VUQNAME) > 255);
    nt_static_assert(sizeof(VDOMAIN) > 255);
    CalculateDomainName(nodename, dnsname, VFQNAME, sizeof(VFQNAME),
                        VUQNAME, sizeof(VUQNAME), VDOMAIN, sizeof(VDOMAIN));

    // Note: We don't expect hostnames or domain names above 255
    // Not supported by DNS:
    // https://tools.ietf.org/html/rfc2181#section-11
    // So let's print some warnings:
    if (strlen(VUQNAME) > 255)
    {
        Log(LOG_LEVEL_WARNING,
            "Long host name '%s' (%zu bytes) will may cause issues",
            VUQNAME,
            strlen(VUQNAME));
    }
    if (strlen(VDOMAIN) > 255)
    {
        Log(LOG_LEVEL_WARNING,
            "Long domain name '%s' (%zu bytes) will may cause issues",
            VDOMAIN,
            strlen(VDOMAIN));
    }

/*
 * VFQNAME = a.b.c.d ->
 * NewClass("a.b.c.d")
 * NewClass("b.c.d")
 * NewClass("c.d")
 * NewClass("d")
 */
    char *ptr = VFQNAME;

    do
    {
        EvalContextClassPutHard(ctx, ptr, "inventory,attribute_name=none,source=agent,derived-from=sys.fqhost");

        ptr = strchr(ptr, '.');
        if (ptr != NULL)
        {
            ptr++;
        }
    } while (ptr != NULL);

    EvalContextClassPutHard(ctx, VUQNAME, "source=agent,derived-from=sys.uqhost");
    EvalContextClassPutHard(ctx, VDOMAIN, "source=agent,derived-from=sys.domain");

    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "host", nodename, CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=none");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "uqhost", VUQNAME, CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=none");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "fqhost", VFQNAME, CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=Host name");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "domain", VDOMAIN, CF_DATA_TYPE_STRING, "source=agent");
}

/*******************************************************************/

void DiscoverVersion(EvalContext *ctx)
{
    int major = 0;
    int minor = 0;
    int patch = 0;
    char workbuf[CF_BUFSIZE];
    if (sscanf(Version(), "%d.%d.%d", &major, &minor, &patch) == 3)
    {
        snprintf(workbuf, CF_MAXVARSIZE, "%d", major);
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cf_version_major", workbuf, CF_DATA_TYPE_STRING, "source=agent");
        snprintf(workbuf, CF_MAXVARSIZE, "%d", minor);
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cf_version_minor", workbuf, CF_DATA_TYPE_STRING, "source=agent");
        snprintf(workbuf, CF_MAXVARSIZE, "%d", patch);
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cf_version_patch", workbuf, CF_DATA_TYPE_STRING, "source=agent");
    }
    else
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cf_version_major", "BAD VERSION " VERSION, CF_DATA_TYPE_STRING, "source=agent");
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cf_version_minor", "BAD VERSION " VERSION, CF_DATA_TYPE_STRING, "source=agent");
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cf_version_patch", "BAD VERSION " VERSION, CF_DATA_TYPE_STRING, "source=agent");
    }

    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cf_version_release", RELEASE, CF_DATA_TYPE_STRING, "source=agent");

    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "local_libdir", "lib", CF_DATA_TYPE_STRING, "source=agent");

    snprintf(workbuf, CF_BUFSIZE, "%s%cinputs%clib", GetWorkDir(), FILE_SEPARATOR, FILE_SEPARATOR);
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "libdir", workbuf, CF_DATA_TYPE_STRING, "source=agent");
}

static void GetNameInfo3(EvalContext *ctx)
{
    int i;
    char *sp, workbuf[CF_BUFSIZE];
    time_t tloc;
    struct hostent *hp;
    struct sockaddr_in cin;
    unsigned char digest[EVP_MAX_MD_SIZE + 1];
    const char* const workdir = GetWorkDir();
    const char* const bindir = GetBinDir();
    const char* const moduledir = GetModuleDir();
    const char* const keydir = GetKeyDir();

#ifdef _AIX
    char real_version[_SYS_NMLN];
#endif
#if defined(HAVE_SYSINFO) && (defined(SI_ARCHITECTURE) || defined(SI_PLATFORM))
    long sz;
#endif

#define COMPONENTS_SIZE 17
    // This is used for $(sys.cf_agent), $(sys.cf_serverd) ... :
    char *components[COMPONENTS_SIZE] = { "cf-twin", "cf-agent", "cf-serverd", "cf-monitord", "cf-know",
        "cf-report", "cf-key", "cf-runagent", "cf-execd", "cf-hub", "cf-reactor",
        "cf-promises", "cf-upgrade", "cf-net", "cf-check", "cf-secret",
        NULL
    };
    int have_component[COMPONENTS_SIZE] = {0};
    struct stat sb;
    char name[CF_MAXVARSIZE], quoteName[CF_MAXVARSIZE + 2], shortname[CF_MAXVARSIZE];

    if (uname(&VSYSNAME) == -1)
    {
        Log(LOG_LEVEL_ERR, "Couldn't get kernel name info!. (uname: %s)", GetErrorStr());
        memset(&VSYSNAME, 0, sizeof(VSYSNAME));
    }

#ifdef _AIX
    snprintf(real_version, _SYS_NMLN, "%.80s.%.80s", VSYSNAME.version, VSYSNAME.release);
    strlcpy(VSYSNAME.release, real_version, _SYS_NMLN);
#endif
#if defined(__ANDROID__) && !defined(__TERMUX__)
    /*
     * uname cannot differentiate android from linux
     */
    strcpy(VSYSNAME.sysname, "android");
#endif
#ifdef __BUSYBOX__
    /*
     * uname cannot differentiate a busybox toolset from a normal GNU linux toolset
     */
     strcpy(VSYSNAME.sysname, "busybox");
#endif

    ToLowerStrInplace(VSYSNAME.sysname);
    ToLowerStrInplace(VSYSNAME.machine);

#ifdef _AIX
    switch (_system_configuration.architecture)
    {
    case POWER_RS:
        strlcpy(VSYSNAME.machine, "power", _SYS_NMLN);
        break;
    case POWER_PC:
        strlcpy(VSYSNAME.machine, "powerpc", _SYS_NMLN);
        break;
    case IA64:
        strlcpy(VSYSNAME.machine, "ia64", _SYS_NMLN);
        break;
    }
#endif

/*
 * solarisx86 is a historically defined class for Solaris on x86. We have to
 * define it manually now.
 */
#ifdef __sun
    if (strcmp(VSYSNAME.machine, "i86pc") == 0)
    {
        EvalContextClassPutHard(ctx, "solarisx86", "inventory,attribute_name=none,source=agent");
    }
#endif

    DetectDomainName(ctx, VSYSNAME.nodename);

    if ((tloc = time((time_t *) NULL)) == -1)
    {
        Log(LOG_LEVEL_ERR, "Couldn't read system clock");
    }
    else
    {
        snprintf(workbuf, CF_BUFSIZE, "%jd", (intmax_t) tloc);
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "systime", workbuf, CF_DATA_TYPE_INT, "time_based,source=agent");
        snprintf(workbuf, CF_BUFSIZE, "%jd", (intmax_t) tloc / SECONDS_PER_DAY);
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "sysday", workbuf, CF_DATA_TYPE_INT, "time_based,source=agent");
        i = GetUptimeMinutes(tloc);
        if (i != -1)
        {
            snprintf(workbuf, CF_BUFSIZE, "%d", i);
            EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "uptime", workbuf, CF_DATA_TYPE_INT, "inventory,time_based,source=agent,attribute_name=Uptime minutes");
        }
    }

    bool found = false;
    for (i = 0; !found && (i < PLATFORM_CONTEXT_MAX); i++)
    {
        char sysname[CF_BUFSIZE];
        strlcpy(sysname, VSYSNAME.sysname, CF_BUFSIZE);
        ToLowerStrInplace(sysname);

        /* FIXME: review those strcmps. Moved out from StringMatch */
        if (!strcmp(CLASSATTRIBUTES[i][0], sysname)
            || StringMatchFull(CLASSATTRIBUTES[i][0], sysname))
        {
            if (!strcmp(CLASSATTRIBUTES[i][1], VSYSNAME.machine)
                || StringMatchFull(CLASSATTRIBUTES[i][1], VSYSNAME.machine))
            {
                if (!strcmp(CLASSATTRIBUTES[i][2], VSYSNAME.release)
                    || StringMatchFull(CLASSATTRIBUTES[i][2], VSYSNAME.release))
                {
                    EvalContextClassPutHard(ctx, CLASSTEXT[i], "inventory,attribute_name=none,source=agent,derived-from=sys.class");

                    found = true;

                    VSYSTEMHARDCLASS = (PlatformContext) i;
                    VPSHARDCLASS = (PlatformContext) i; /* this one can be overriden at vz detection */
                    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "class", CLASSTEXT[i], CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=Kernel");
                }
            }
            else
            {
                Log(LOG_LEVEL_DEBUG, "I recognize '%s' but not '%s'", VSYSNAME.sysname, VSYSNAME.machine);
                continue;
            }
        }
    }

    Log(LOG_LEVEL_VERBOSE, "%s - ready", NameVersion());
    Banner("Environment discovery");

    snprintf(workbuf, CF_BUFSIZE, "%s", CLASSTEXT[VSYSTEMHARDCLASS]);

    Log(LOG_LEVEL_VERBOSE, "Host name is: %s", VSYSNAME.nodename);
    Log(LOG_LEVEL_VERBOSE, "Operating System Type is %s", VSYSNAME.sysname);
    Log(LOG_LEVEL_VERBOSE, "Operating System Release is %s", VSYSNAME.release);
    Log(LOG_LEVEL_VERBOSE, "Architecture = %s", VSYSNAME.machine);
    Log(LOG_LEVEL_VERBOSE, "CFEngine detected operating system description is %s", workbuf);
    Log(LOG_LEVEL_VERBOSE, "The time is now %s", ctime(&tloc));

    snprintf(workbuf, CF_MAXVARSIZE, "%s", ctime(&tloc));
    Chop(workbuf, CF_EXPANDSIZE);

    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "date", workbuf, CF_DATA_TYPE_STRING, "time_based,source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cdate", CanonifyName(workbuf), CF_DATA_TYPE_STRING, "time_based,source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "os", VSYSNAME.sysname, CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "release", VSYSNAME.release, CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=Kernel Release");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "version", VSYSNAME.version, CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "arch", VSYSNAME.machine, CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=Architecture");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "workdir", workdir, CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "moduledir", moduledir, CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "fstab", VFSTAB[VSYSTEMHARDCLASS], CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "resolv", VRESOLVCONF[VSYSTEMHARDCLASS], CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "maildir", VMAILDIR[VSYSTEMHARDCLASS], CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "exports", VEXPORTS[VSYSTEMHARDCLASS], CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "logdir", GetLogDir(), CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "piddir", GetPidDir(), CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "statedir", GetStateDir(), CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "masterdir", GetMasterDir(), CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "inputdir", GetInputDir(), CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "keydir", keydir, CF_DATA_TYPE_STRING, "source=agent");

    snprintf(workbuf, CF_BUFSIZE, "%s", bindir);
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "bindir", workbuf, CF_DATA_TYPE_STRING, "source=agent");

    snprintf(workbuf, CF_BUFSIZE, "%s%cpromises.cf", GetInputDir(), FILE_SEPARATOR);
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "default_policy_path", workbuf, CF_DATA_TYPE_STRING, "source=agent");

    snprintf(workbuf, CF_BUFSIZE, "%s%cfailsafe.cf", GetInputDir(), FILE_SEPARATOR);
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "failsafe_policy_path", workbuf, CF_DATA_TYPE_STRING, "source=agent");

    snprintf(workbuf, CF_BUFSIZE, "%s%cupdate.cf", GetInputDir(), FILE_SEPARATOR);
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "update_policy_path", workbuf, CF_DATA_TYPE_STRING, "source=agent");

/* FIXME: type conversion */
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cf_version", (char *) Version(), CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=CFEngine version");

    DiscoverVersion(ctx);

    if (PUBKEY)
    {
        char pubkey_digest[CF_HOSTKEY_STRING_SIZE] = { 0 };

        HashPubKey(PUBKEY, digest, CF_DEFAULT_DIGEST);
        HashPrintSafe(pubkey_digest, sizeof(pubkey_digest), digest,
                      CF_DEFAULT_DIGEST, true);

        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "key_digest", pubkey_digest, CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=CFEngine ID");

        snprintf(workbuf, CF_MAXVARSIZE - 1, "PK_%s", pubkey_digest);
        CanonifyNameInPlace(workbuf);
        EvalContextClassPutHard(ctx, workbuf, "inventory,attribute_name=none,source=agent,derived-from=sys.key_digest");
    }

    for (i = 0; components[i] != NULL; i++)
    {
        snprintf(shortname, CF_MAXVARSIZE - 1, "%s", CanonifyName(components[i]));

#if defined(_WIN32)
        // twin has own dir, and is named agent
        if (i == 0)
        {
            snprintf(name, CF_MAXVARSIZE - 1, "%s-twin%ccf-agent.exe", bindir, FILE_SEPARATOR);
        }
        else
        {
            snprintf(name, CF_MAXVARSIZE - 1, "%s%c%s.exe", bindir, FILE_SEPARATOR, components[i]);
        }
#else
        snprintf(name, CF_MAXVARSIZE - 1, "%s%c%s", bindir, FILE_SEPARATOR, components[i]);
#endif

        have_component[i] = false;

        if (stat(name, &sb) != -1)
        {
            snprintf(quoteName, sizeof(quoteName), "\"%s\"", name);
            // Sets $(sys.cf_agent), $(sys.cf_serverd) $(sys.cf_execd) etc.
            // to their respective /bin paths
            EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, shortname, quoteName, CF_DATA_TYPE_STRING, "cfe_internal,source=agent");
            have_component[i] = true;
        }
    }

// If no twin, fail over the agent

    if (!have_component[0])
    {
        snprintf(shortname, CF_MAXVARSIZE - 1, "%s", CanonifyName(components[0]));

#if defined(_WIN32)
        snprintf(name, CF_MAXVARSIZE - 1, "%s%c%s.exe", bindir, FILE_SEPARATOR,
                 components[1]);
#else
        snprintf(name, CF_MAXVARSIZE - 1, "%s%c%s", bindir, FILE_SEPARATOR, components[1]);
#endif

        if (stat(name, &sb) != -1)
        {
            snprintf(quoteName, sizeof(quoteName), "\"%s\"", name);
            EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, shortname, quoteName, CF_DATA_TYPE_STRING, "cfe_internal,source=agent");
        }
    }

/* Windows special directories and tools */

#ifdef __MINGW32__
    if (NovaWin_GetWinDir(workbuf, sizeof(workbuf)))
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "windir", workbuf, CF_DATA_TYPE_STRING, "source=agent");
    }

    if (NovaWin_GetSysDir(workbuf, sizeof(workbuf)))
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "winsysdir", workbuf, CF_DATA_TYPE_STRING, "source=agent");

        char filename[CF_BUFSIZE];
        if (snprintf(filename, sizeof(filename), "%s%s", workbuf, "\\WindowsPowerShell\\v1.0\\powershell.exe") < sizeof(filename))
        {
            if (NovaWin_FileExists(filename))
            {
                EvalContextClassPutHard(ctx, "powershell", "inventory,attribute_name=none,source=agent");
                Log(LOG_LEVEL_VERBOSE, "Additional hard class defined as: %s", "powershell");
            }
        }
    }

    if (NovaWin_GetProgDir(workbuf, sizeof(workbuf)))
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "winprogdir", workbuf, CF_DATA_TYPE_STRING, "source=agent");
    }

# ifdef _WIN64
// only available on 64 bit windows systems
    if (NovaWin_GetEnv("PROGRAMFILES(x86)", workbuf, sizeof(workbuf)))
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "winprogdir86", workbuf, CF_DATA_TYPE_STRING, "source=agent");
    }

# else/* NOT _WIN64 */

    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "winprogdir86", "", CF_DATA_TYPE_STRING, "source=agent");

# endif
#endif /* !__MINGW32__ */

    EnterpriseContext(ctx);

    snprintf(workbuf, sizeof(workbuf), "%u_bit", (unsigned) sizeof(void*) * 8);
    EvalContextClassPutHard(ctx, workbuf, "source=agent");
    Log(LOG_LEVEL_VERBOSE, "Additional hard class defined as: %s", CanonifyName(workbuf));

    snprintf(workbuf, CF_BUFSIZE, "%s_%s", VSYSNAME.sysname, VSYSNAME.release);
    EvalContextClassPutHard(ctx, workbuf, "inventory,attribute_name=none,source=agent,derived-from=sys.sysname,derived-from=sys.release");

    EvalContextClassPutHard(ctx, VSYSNAME.machine, "source=agent,derived-from=sys.machine");
    Log(LOG_LEVEL_VERBOSE, "Additional hard class defined as: %s", CanonifyName(workbuf));

    snprintf(workbuf, CF_BUFSIZE, "%s_%s", VSYSNAME.sysname, VSYSNAME.machine);
    EvalContextClassPutHard(ctx, workbuf, "source=agent,derived-from=sys.sysname,derived-from=sys.machine");
    Log(LOG_LEVEL_VERBOSE, "Additional hard class defined as: %s", CanonifyName(workbuf));

    snprintf(workbuf, CF_BUFSIZE, "%s_%s_%s", VSYSNAME.sysname, VSYSNAME.machine, VSYSNAME.release);
    EvalContextClassPutHard(ctx, workbuf, "source=agent,derived-from=sys.sysname,derived-from=sys.machine,derived-from=sys.release");
    Log(LOG_LEVEL_VERBOSE, "Additional hard class defined as: %s", CanonifyName(workbuf));

#ifdef HAVE_SYSINFO
# ifdef SI_ARCHITECTURE
    sz = sysinfo(SI_ARCHITECTURE, workbuf, CF_BUFSIZE);
    if (sz == -1)
    {
        Log(LOG_LEVEL_VERBOSE, "cfengine internal: sysinfo returned -1");
    }
    else
    {
        EvalContextClassPutHard(ctx, workbuf, "inventory,attribute_name=none,source=agent");
        Log(LOG_LEVEL_VERBOSE, "Additional hard class defined as: %s", workbuf);
    }
# endif
# ifdef SI_PLATFORM
    sz = sysinfo(SI_PLATFORM, workbuf, CF_BUFSIZE);
    if (sz == -1)
    {
        Log(LOG_LEVEL_VERBOSE, "cfengine internal: sysinfo returned -1");
    }
    else
    {
        EvalContextClassPutHard(ctx, workbuf, "inventory,attribute_name=none,source=agent");
        Log(LOG_LEVEL_VERBOSE, "Additional hard class defined as: %s", workbuf);
    }
# endif
#endif

    snprintf(workbuf, CF_BUFSIZE, "%s_%s_%s_%s", VSYSNAME.sysname, VSYSNAME.machine, VSYSNAME.release,
             VSYSNAME.version);

    if (strlen(workbuf) > CF_MAXVARSIZE - 2)
    {
        Log(LOG_LEVEL_VERBOSE, "cfengine internal: $(arch) overflows CF_MAXVARSIZE! Truncating");
    }

    sp = xstrdup(CanonifyName(workbuf));
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "long_arch", sp, CF_DATA_TYPE_STRING, "source=agent");
    EvalContextClassPutHard(ctx, sp, "source=agent,derived-from=sys.long_arch");
    free(sp);

    snprintf(workbuf, CF_BUFSIZE, "%s_%s", VSYSNAME.sysname, VSYSNAME.machine);
    sp = xstrdup(CanonifyName(workbuf));
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "ostype", sp, CF_DATA_TYPE_STRING, "source=agent");
    EvalContextClassPutHard(ctx, sp, "inventory,attribute_name=none,source=agent,derived-from=sys.ostype");
    free(sp);

    if (!found)
    {
        Log(LOG_LEVEL_ERR, "I don't understand what architecture this is");
    }

    char compile_str[] = "compiled_on_";
    size_t compile_str_len = sizeof(compile_str);
    strcpy(workbuf, compile_str);

    strlcat(workbuf, CanonifyName(AUTOCONF_SYSNAME),
            CF_BUFSIZE - compile_str_len);
    EvalContextClassPutHard(ctx, workbuf, "source=agent");
    Log(LOG_LEVEL_VERBOSE, "GNU autoconf class from compile time: %s", workbuf);

/* Get IP address from nameserver */

    if ((hp = gethostbyname(VFQNAME)) == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "Hostname lookup failed on node name '%s'", VSYSNAME.nodename);
        return;
    }
    else
    {
        memset(&cin, 0, sizeof(cin));
        cin.sin_addr.s_addr = ((struct in_addr *) (hp->h_addr))->s_addr;
        Log(LOG_LEVEL_VERBOSE, "Address given by nameserver: %s", inet_ntoa(cin.sin_addr));
        strcpy(VIPADDRESS, inet_ntoa(cin.sin_addr));

        for (i = 0; hp->h_aliases[i] != NULL; i++)
        {
            Log(LOG_LEVEL_DEBUG, "Adding alias '%s'", hp->h_aliases[i]);
            EvalContextClassPutHard(ctx, hp->h_aliases[i], "inventory,attribute_name=none,source=agent,based-on=sys.fqhost");
        }
    }

#ifdef HAVE_GETZONEID
    zoneid_t zid;
    char zone[ZONENAME_MAX];
    char vbuff[CF_BUFSIZE];

    zid = getzoneid();
    getzonenamebyid(zid, zone, ZONENAME_MAX);

    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "zone", zone, CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=Solaris zone");
    snprintf(vbuff, CF_BUFSIZE - 1, "zone_%s", zone);
    EvalContextClassPutHard(ctx, vbuff, "source=agent,derived-from=sys.zone");

    if (strcmp(zone, "global") == 0)
    {
        Log(LOG_LEVEL_VERBOSE, "CFEngine seems to be running inside a global solaris zone of name '%s'", zone);
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "CFEngine seems to be running inside a local solaris zone of name '%s'", zone);
    }
#endif
}

/*******************************************************************/

void LoadSlowlyVaryingObservations(EvalContext *ctx)
{
    CF_DB *dbp;
    CF_DBC *dbcp;
    char *key;
    void *stored;
    int ksize, vsize;

    if (!OpenDB(&dbp, dbid_static))
    {
        return;
    }

    /* Acquire a cursor for the database. */
    if (!NewDBCursor(dbp, &dbcp))
    {
        Log(LOG_LEVEL_INFO, "Unable to scan class db");
        CloseDB(dbp);
        return;
    }

    while (NextDB(dbcp, &key, &ksize, &stored, &vsize))
    {
        if (key == NULL  ||  stored == NULL)
        {
            continue;
        }

        char lval[1024];
        int type_i;
        int ret = sscanf(key, "%1023[^:]:%d", lval, &type_i);
        if (ret == 2)
        {
            DataType type = type_i;
            switch (type)
            {
            case CF_DATA_TYPE_STRING:
            case CF_DATA_TYPE_INT:
            case CF_DATA_TYPE_REAL:
                EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_MON,
                                              lval, stored, type,
                                              "monitoring,source=observation");
                break;

            case CF_DATA_TYPE_STRING_LIST:
            {
                Rlist *list = RlistFromSplitString(stored, ',');
                EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_MON,
                                              lval, list, CF_DATA_TYPE_STRING_LIST,
                                              "monitoring,source=observation");
                RlistDestroy(list);
                break;
            }
            case CF_DATA_TYPE_COUNTER:
                EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_MON,
                                              lval, stored, CF_DATA_TYPE_STRING,
                                              "monitoring,source=observation");
                break;

            default:
                Log(LOG_LEVEL_ERR,
                    "Unexpected monitoring type %d found in dbid_static database",
                    (int) type);
            }
        }
    }

    DeleteDBCursor(dbcp);
    CloseDB(dbp);
}

static void Get3Environment(EvalContext *ctx)
{
    char env[CF_BUFSIZE], context[CF_BUFSIZE], name[CF_MAXVARSIZE], value[CF_BUFSIZE];
    struct stat statbuf;
    time_t now = time(NULL);

    Log(LOG_LEVEL_VERBOSE, "Looking for environment from cf-monitord...");

    snprintf(env, CF_BUFSIZE, "%s/%s", GetStateDir(), CF_ENV_FILE);
    MapName(env);

    FILE *fp = safe_fopen(env, "r");
    if (fp == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "Unable to detect environment from cf-monitord");
        return;
    }

    int fd = fileno(fp);
    if (fstat(fd, &statbuf) == -1)
    {
        Log(LOG_LEVEL_VERBOSE, "Unable to detect environment from cf-monitord");
        fclose(fp);
        return;
    }

    if (statbuf.st_mtime < (now - 60 * 60))
    {
        Log(LOG_LEVEL_VERBOSE, "Environment data are too old - discarding");
        unlink(env);
        fclose(fp);
        return;
    }

    snprintf(value, CF_MAXVARSIZE - 1, "%s", ctime(&statbuf.st_mtime));
    if (Chop(value, CF_EXPANDSIZE) == -1)
    {
        Log(LOG_LEVEL_ERR, "Chop was called on a string that seemed to have no terminator");
    }

    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_MON, "env_time", value, CF_DATA_TYPE_STRING, "time_based,source=agent");

    Log(LOG_LEVEL_VERBOSE, "Loading environment...");

    for(;;)
    {
        name[0] = '\0';
        value[0] = '\0';

        if (fgets(context, sizeof(context), fp) == NULL)
        {
            if (ferror(fp))
            {
                UnexpectedError("Failed to read line from stream");
                break;
            }
            else /* feof */
            {
                break;
            }
        }


        if (*context == '@')
        {
            nt_static_assert((sizeof(name)) > 255);
            nt_static_assert((sizeof(value)) > 255);
            if (sscanf(context + 1, "%255[^=]=%255[^\n]", name, value) == 2)
            {
                Log(LOG_LEVEL_DEBUG,
                    "Setting new monitoring list '%s' => '%s'",
                    name, value);
                Rlist *list = RlistParseShown(value);
                EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_MON,
                                              name, list,
                                              CF_DATA_TYPE_STRING_LIST,
                                              "monitoring,source=environment");

                RlistDestroy(list);
            }
            else
            {
                Log(LOG_LEVEL_ERR,
                    "Failed to parse '%s' as '@variable=list' monitoring list",
                    context);
            }
        }
        else if (strchr(context, '='))
        {
            nt_static_assert((sizeof(name)) > 255);
            nt_static_assert((sizeof(value)) > 255);
            if (sscanf(context, "%255[^=]=%255[^\n]", name, value) == 2)
            {
                EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_MON,
                                              name, value,
                                              CF_DATA_TYPE_STRING,
                                              "monitoring,source=environment");
                Log(LOG_LEVEL_DEBUG,
                    "Setting new monitoring scalar '%s' => '%s'",
                    name, value);
            }
            else
            {
                Log(LOG_LEVEL_ERR,
                    "Failed to parse '%s' as 'variable=value' monitoring scalar",
                    context);
            }
        }
        else
        {
            StripTrailingNewline(context, CF_BUFSIZE);
            EvalContextClassPutHard(ctx, context, "monitoring,source=environment");
        }
    }

    fclose(fp);
    Log(LOG_LEVEL_VERBOSE, "Environment data loaded");

    LoadSlowlyVaryingObservations(ctx);
}

static void BuiltinClasses(EvalContext *ctx)
{
    char vbuff[CF_BUFSIZE];

    EvalContextClassPutHard(ctx, "any", "source=agent");            /* This is a reserved word / wildcard */

    snprintf(vbuff, CF_BUFSIZE, "cfengine_%s", CanonifyName(Version()));
    CreateHardClassesFromCanonification(ctx, vbuff, "inventory,attribute_name=none,source=agent");

    CreateHardClassesFromFeatures(ctx, "source=agent");
}

/*******************************************************************/

void CreateHardClassesFromCanonification(EvalContext *ctx, const char *canonified, char *tags)
{
    char buf[CF_MAXVARSIZE];

    strlcpy(buf, canonified, sizeof(buf));

    EvalContextClassPutHard(ctx, buf, tags);

    char *sp;

    while ((sp = strrchr(buf, '_')))
    {
        *sp = 0;
        EvalContextClassPutHard(ctx, buf, tags);
    }
}

static void SetFlavor(EvalContext *ctx, const char *flavor)
{
    EvalContextClassPutHard(ctx, flavor, "inventory,attribute_name=none,source=agent,derived-from=sys.flavor");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "flavour", flavor, CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "flavor", flavor, CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=none");
}

#if defined __linux__ || __MINGW32__

/**
 * @brief Combines OS and version string to define flavor variable and class
 *
 * @note Input strings should be canonified before calling
 */
static void SetFlavor2(
    EvalContext *const ctx,
    const char *const id,
    const char *const major_version)
{
    assert(ctx != NULL);
    assert(id != NULL);
    assert(major_version != NULL);

    char *flavor;
    xasprintf(&flavor, "%s_%s", id, major_version);
    SetFlavor(ctx, flavor);
    free(flavor);
}

#endif

#ifdef __linux__

/**
 * @brief Combines OS and version string to define multiple hard classes
 *
 * @note Input strings should be canonified before calling
 */
static void DefineVersionedHardClasses(
    EvalContext *const ctx,
    const char *const tags,
    const char *const id,
    const char *const version)
{
    assert(ctx != NULL);
    assert(id != NULL);
    assert(version != NULL);

    char *class;
    xasprintf(&class, "%s_%s", id, version);

    // Strip away version number to define multiple hard classes
    // Example: coreos_1185_3_0 -> coreos_1185_3 -> coreos_1185 -> coreos
    char *last_underscore = strrchr(class, '_');
    while ( last_underscore != NULL )
    {
        EvalContextClassPutHard(ctx, class, tags);
        *last_underscore = '\0';
        last_underscore = strrchr(class, '_');
    }
    EvalContextClassPutHard(ctx, class, tags);
    free(class);
}

static void OSReleaseParse(EvalContext *ctx, const char *file_path)
{
    JsonElement *os_release_json = JsonReadDataFile("system info discovery",
                                                    file_path, DATAFILETYPE_ENV,
                                                    100 * 1024);
    if (os_release_json != NULL)
    {
        char *tags;
        xasprintf(&tags,
                  "inventory,attribute_name=none,source=agent,derived-from-file=%s",
                  file_path);
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "os_release",
                                      os_release_json, CF_DATA_TYPE_CONTAINER,
                                      tags);
        const char *const_os_release_id = JsonObjectGetAsString(os_release_json, "ID");
        const char *const_os_release_version_id = JsonObjectGetAsString(os_release_json, "VERSION_ID");
        char *os_release_id = SafeStringDuplicate(const_os_release_id);
        char *os_release_version_id = SafeStringDuplicate(const_os_release_version_id);

        if (os_release_id != NULL)
        {
            CanonifyNameInPlace(os_release_id);

            const char *alias = NULL;
            if (StringEqual(os_release_id, "rhel"))
            {
                alias = "redhat";
            }
            else if (StringEqual(os_release_id, "opensuse") ||
                     StringEqual(os_release_id, "sles") ||
                     StringEqual(os_release_id, "sled"))
            {
                alias = "suse";
            }

            if (os_release_version_id == NULL)
            {
                // if VERSION_ID doesn't exist, define only one hard class:
                EvalContextClassPutHard(ctx, os_release_id, tags);
                if (alias != NULL)
                {
                    EvalContextClassPutHard(ctx, alias, tags);
                }
            }
            else // if VERSION_ID exists set flavor and multiple hard classes:
            {
                CanonifyNameInPlace(os_release_version_id);

                // Set the flavor to be ID + major version (derived from VERSION_ID)
                char *first_underscore = strchr(os_release_version_id, '_');
                if (first_underscore != NULL)
                {
                    // Temporarily modify os_release_version_id to be major version
                    *first_underscore = '\0';
                    SetFlavor2(ctx, os_release_id, os_release_version_id);
                    *first_underscore = '_';
                }
                else
                {
                    SetFlavor2(ctx, os_release_id, os_release_version_id);
                }

                // One of the hard classes is already set by SetFlavor
                // but it seems excessive to try to skip this:
                DefineVersionedHardClasses(ctx, tags, os_release_id, os_release_version_id);
                if (alias != NULL)
                {
                    DefineVersionedHardClasses(ctx, tags, alias, os_release_version_id);
                }
            }
        }
        free(os_release_version_id);
        free(os_release_id);
        free(tags);
        JsonDestroy(os_release_json);
    }
}
#endif

static void OSClasses(EvalContext *ctx)
{
#ifdef __linux__

/* First we check if init process is systemd, and set "systemd" hard class. */

    {
        char init_path[CF_BUFSIZE];
        if (ReadLine("/proc/1/cmdline", init_path, sizeof(init_path)))
        {
            /* Follow possible symlinks. */

            char resolved_path[PATH_MAX];      /* realpath() needs PATH_MAX */
            if (realpath(init_path, resolved_path) != NULL &&
                strlen(resolved_path) < sizeof(init_path))
            {
                strcpy(init_path, resolved_path);
            }

            /* Check if string ends with "/systemd". */
            char *p;
            char *next_p = NULL;
            const char *term = "/systemd";
            do
            {
                p = next_p;
                next_p = strstr(next_p ? next_p+strlen(term) : init_path, term);
            }
            while (next_p);

            if (p != NULL &&
                p[strlen("/systemd")] == '\0')
            {
                EvalContextClassPutHard(ctx, "systemd",
                                        "inventory,attribute_name=none,source=agent");
            }
        }
    }


    struct stat statbuf;

    // os-release is used to set sys.os_release, sys.flavor and hard classes
    if (access("/etc/os-release", R_OK) != -1)
    {
        OSReleaseParse(ctx, "/etc/os-release");
    }
    else if (access("/usr/lib/os-release", R_OK) != -1)
    {
        OSReleaseParse(ctx, "/usr/lib/os-release");
    }

/* Mandrake/Mandriva, Fedora and Oracle VM Server supply /etc/redhat-release, so
   we test for those distributions first */

    if (stat("/etc/mandriva-release", &statbuf) != -1)
    {
        Linux_Mandriva_Version(ctx);
    }
    else if (stat("/etc/mandrake-release", &statbuf) != -1)
    {
        Linux_Mandrake_Version(ctx);
    }
    else if (stat("/etc/fedora-release", &statbuf) != -1)
    {
        Linux_Fedora_Version(ctx);
    }
    else if (stat("/etc/ovs-release", &statbuf) != -1)
    {
        Linux_Oracle_VM_Server_Version(ctx);
    }
    else if (stat("/etc/redhat-release", &statbuf) != -1)
    {
        Linux_Redhat_Version(ctx);
    }

/* Oracle Linux >= 6 supplies separate /etc/oracle-release alongside
   /etc/redhat-release, use it to precisely identify version */

    if (stat("/etc/oracle-release", &statbuf) != -1)
    {
        Linux_Oracle_Version(ctx);
    }

    if (stat("/etc/generic-release", &statbuf) != -1)
    {
        Log(LOG_LEVEL_VERBOSE, "This appears to be a sun cobalt system.");
        SetFlavor(ctx, "SunCobalt");
    }

    if (stat("/etc/SuSE-release", &statbuf) != -1)
    {
        Linux_Suse_Version(ctx);
    }

    if (stat("/etc/system-release", &statbuf) != -1)
    {
        Linux_Amazon_Version(ctx);
    }

# define SLACKWARE_ANCIENT_VERSION_FILENAME "/etc/slackware-release"
# define SLACKWARE_VERSION_FILENAME "/etc/slackware-version"
    if (stat(SLACKWARE_VERSION_FILENAME, &statbuf) != -1)
    {
        Linux_Slackware_Version(ctx, SLACKWARE_VERSION_FILENAME);
    }
    else if (stat(SLACKWARE_ANCIENT_VERSION_FILENAME, &statbuf) != -1)
    {
        Linux_Slackware_Version(ctx, SLACKWARE_ANCIENT_VERSION_FILENAME);
    }

    if (stat(DEBIAN_VERSION_FILENAME, &statbuf) != -1)
    {
        Linux_Debian_Version(ctx);
    }

    if (stat(LSB_RELEASE_FILENAME, &statbuf) != -1)
    {
        Linux_Misc_Version(ctx);
    }

    if (stat("/usr/bin/aptitude", &statbuf) != -1)
    {
        Log(LOG_LEVEL_VERBOSE, "This system seems to have the aptitude package system");
        EvalContextClassPutHard(ctx, "have_aptitude", "inventory,attribute_name=none,source=agent");
    }

    if (stat("/etc/UnitedLinux-release", &statbuf) != -1)
    {
        Log(LOG_LEVEL_VERBOSE, "This appears to be a UnitedLinux system.");
        SetFlavor(ctx, "UnitedLinux");
    }

    if (stat("/etc/alpine-release", &statbuf) != -1)
    {
        Linux_Alpine_Version(ctx);
    }

    if (stat("/etc/gentoo-release", &statbuf) != -1)
    {
        Log(LOG_LEVEL_VERBOSE, "This appears to be a gentoo system.");
        SetFlavor(ctx, "gentoo");
    }

    if (stat("/etc/manjaro-release", &statbuf) != -1)
    {
        Log(LOG_LEVEL_VERBOSE, "This appears to be a Manjaro Linux system.");
        SetFlavor(ctx, "manjaro");
    }
    else if (stat("/etc/arch-release", &statbuf) != -1)
    {
        Log(LOG_LEVEL_VERBOSE, "This appears to be an Arch Linux system.");
        SetFlavor(ctx, "archlinux");
    }

    if (stat("/proc/vmware/version", &statbuf) != -1 || stat("/etc/vmware-release", &statbuf) != -1)
    {
        VM_Version(ctx);
    }
    else if (stat("/etc/vmware", &statbuf) != -1 && S_ISDIR(statbuf.st_mode))
    {
        VM_Version(ctx);
    }

    if (stat("/proc/xen/capabilities", &statbuf) != -1)
    {
        Xen_Domain(ctx);
    }
    if (stat("/etc/Eos-release", &statbuf) != -1)
    {
        EOS_Version(ctx);
        SetFlavor(ctx, "Eos");
    }

    if (stat("/etc/issue", &statbuf) != -1)
    {
        MiscOS(ctx);
    }

    if (stat("/proc/self/status", &statbuf) != -1)
    {
        OpenVZ_Detect(ctx);
    }

#else

#ifdef __APPLE__
    Apple_Version(ctx);
#endif

    char vbuff[CF_MAXVARSIZE];

#ifdef _AIX
    strlcpy(vbuff, VSYSNAME.version, CF_MAXVARSIZE);
#else
    strlcpy(vbuff, VSYSNAME.release, CF_MAXVARSIZE);
#endif


#ifndef __APPLE__
    for (char *sp = vbuff; *sp != '\0'; sp++)
    {
        if (*sp == '-')
        {
            *sp = '\0';
            break;
        }
    }

    char context[CF_BUFSIZE];
    snprintf(context, CF_BUFSIZE, "%s_%s", VSYSNAME.sysname, vbuff);
    SetFlavor(ctx, context);
#endif


#ifdef __hpux
    /*
     * Define a hard class with just the version major number of HP-UX
     *
     * For example, when being run on HP-UX B.11.23 the following class will
     * be defined: hpux_11
     */

    // Extract major version number
    char *major = NULL;
    for (char *sp = vbuff; *sp != '\0'; sp++)
    {
        if (major == NULL && isdigit(*sp))
        {
            major = sp;
        }
        else if (!isdigit(*sp))
        {
            *sp = '\0';
        }
    }

    if (major != NULL)
    {
        snprintf(context, CF_BUFSIZE, "hpux_%s", major);
        EvalContextClassPutHard(ctx, context, "source=agent,derived-from=sys.flavor");
    }
#endif

#ifdef __FreeBSD__
    /*
     * Define a hard class with just the version major number on FreeBSD
     *
     * For example, when being run on either FreeBSD 10.0 or 10.1 a class
     * called freebsd_10 will be defined
     */
    for (char *sp = vbuff; *sp != '\0'; sp++)
    {
        if (*sp == '.')
        {
            *sp = '\0';
            break;
        }
    }

    snprintf(context, CF_BUFSIZE, "%s_%s", VSYSNAME.sysname, vbuff);
    EvalContextClassPutHard(ctx, context, "source=agent,derived-from=sys.flavor");
#endif

#endif

#ifdef XEN_CPUID_SUPPORT
    if (Xen_Hv_Check())
    {
        Log(LOG_LEVEL_VERBOSE, "This appears to be a xen hv system.");
        EvalContextClassPutHard(ctx, "xen", "inventory,attribute_name=Virtual host,source=agent");
        EvalContextClassPutHard(ctx, "xen_domu_hv", "source=agent");
    }
#endif /* XEN_CPUID_SUPPORT */

    GetCPUInfo(ctx);

#ifdef __CYGWIN__

    for (char *sp = VSYSNAME.sysname; *sp != '\0'; sp++)
    {
        if (*sp == '-')
        {
            sp++;
            if (strncmp(sp, "5.0", 3) == 0)
            {
                Log(LOG_LEVEL_VERBOSE, "This appears to be Windows 2000");
                EvalContextClassPutHard(ctx, "Win2000", "inventory,attribute_name=none,source=agent");
            }

            if (strncmp(sp, "5.1", 3) == 0)
            {
                Log(LOG_LEVEL_VERBOSE, "This appears to be Windows XP");
                EvalContextClassPutHard(ctx, "WinXP", "inventory,attribute_name=none,source=agent");
            }

            if (strncmp(sp, "5.2", 3) == 0)
            {
                Log(LOG_LEVEL_VERBOSE, "This appears to be Windows Server 2003");
                EvalContextClassPutHard(ctx, "WinServer2003", "inventory,attribute_name=none,source=agent");
            }

            if (strncmp(sp, "6.1", 3) == 0)
            {
                Log(LOG_LEVEL_VERBOSE, "This appears to be Windows Vista");
                EvalContextClassPutHard(ctx, "WinVista", "inventory,attribute_name=none,source=agent");
            }

            if (strncmp(sp, "6.3", 3) == 0)
            {
                Log(LOG_LEVEL_VERBOSE, "This appears to be Windows Server 2008");
                EvalContextClassPutHard(ctx, "WinServer2008", "inventory,attribute_name=none,source=agent");
            }
        }
    }

    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "crontab", "", CF_DATA_TYPE_STRING, "source=agent");

#endif /* __CYGWIN__ */

#ifdef __MINGW32__
    EvalContextClassPutHard(ctx, VSYSNAME.release, "inventory,attribute_name=none,source=agent,derived-from=sys.release"); // code name - e.g. Windows Vista
    EvalContextClassPutHard(ctx, VSYSNAME.version, "inventory,attribute_name=none,source=agent,derived-from=sys.version"); // service pack number - e.g. Service Pack 3

    if (strstr(VSYSNAME.sysname, "workstation"))
    {
        EvalContextClassPutHard(ctx, "WinWorkstation", "inventory,attribute_name=Windows roles,source=agent,derived-from=sys.sysname");
    }
    else if (strstr(VSYSNAME.sysname, "server"))
    {
        EvalContextClassPutHard(ctx, "WinServer", "inventory,attribute_name=Windows roles,source=agent,derived-from=sys.sysname");
    }
    else if (strstr(VSYSNAME.sysname, "domain controller"))
    {
        EvalContextClassPutHard(ctx, "DomainController", "inventory,attribute_name=Windows roles,source=agent,derived-from=sys.sysname");
        EvalContextClassPutHard(ctx, "WinServer", "inventory,attribute_name=Windows roles,source=agent,derived-from=sys.sysname");
    }
    else
    {
        EvalContextClassPutHard(ctx, "unknown_ostype", "source=agent,derived-from=sys.sysname");
    }

    char *release = SafeStringDuplicate(VSYSNAME.release);
    char *major = NULL;
    FindNextInteger(release, &major);
    if (NULL_OR_EMPTY(major))
    {
        SetFlavor(ctx, "windows");
    }
    else
    {
        SetFlavor2(ctx, "windows", major);
    }
    free(release);

#endif /* __MINGW32__ */

#ifndef _WIN32
    char user_name[CF_SMALLBUF];
    if (GetCurrentUserName(user_name, sizeof(user_name)))
    {
        char vbuff[CF_BUFSIZE];

        if (EvalContextClassGet(ctx, NULL, "SUSE"))
        {
            snprintf(vbuff, CF_BUFSIZE, "/var/spool/cron/tabs/%s", user_name);
        }
        else if (EvalContextClassGet(ctx, NULL, "redhat"))
        {
            snprintf(vbuff, CF_BUFSIZE, "/var/spool/cron/%s", user_name);
        }
        else if (EvalContextClassGet(ctx, NULL, "freebsd"))
        {
            snprintf(vbuff, CF_BUFSIZE, "/var/cron/tabs/%s", user_name);
        }
        else if (EvalContextClassGet(ctx, NULL, "macos"))
        {
            snprintf(vbuff, CF_BUFSIZE, "/usr/lib/cron/tabs/%s", user_name);
        }
        else
        {
            snprintf(vbuff, CF_BUFSIZE, "/var/spool/cron/crontabs/%s", user_name);
        }

        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "crontab", vbuff, CF_DATA_TYPE_STRING, "source=agent");
    }

#endif

#if defined(__ANDROID__)
    SetFlavor(ctx, "android");
#endif

/* termux flavor should over-ride android flavor */
#if defined(__TERMUX__)
    SetFlavor(ctx, "termux");
#endif

#ifdef __sun
    if (StringMatchFull("joyent.*", VSYSNAME.version))
    {
        EvalContextClassPutHard(ctx, "smartos", "inventory,attribute_name=none,source=agent,derived-from=sys.version");
        EvalContextClassPutHard(ctx, "smartmachine", "source=agent,derived-from=sys.version");
    }
#endif

    /* FIXME: this variable needs redhat/SUSE/debian classes to be defined and
     * hence can't be initialized earlier */

    if (EvalContextClassGet(ctx, NULL, "redhat"))
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "doc_root", "/var/www/html", CF_DATA_TYPE_STRING, "source=agent");
    }

    if (EvalContextClassGet(ctx, NULL, "SUSE"))
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "doc_root", "/srv/www/htdocs", CF_DATA_TYPE_STRING, "source=agent");
    }

    if (EvalContextClassGet(ctx, NULL, "debian"))
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "doc_root", "/var/www", CF_DATA_TYPE_STRING, "source=agent");
    }
}

/*********************************************************************************/

#ifdef __APPLE__
static void Apple_Version(EvalContext *ctx)
{
    FILE *pp = NULL;

    Log(LOG_LEVEL_VERBOSE, "This appears to be an apple system.");
    Log(LOG_LEVEL_VERBOSE, "Looking for product name and version...");
    if ((!FileCanOpen("/usr/bin/sw_vers", "r") || ((pp = cf_popen("/usr/bin/sw_vers", "r", true)) == NULL)))
    {
        Log(LOG_LEVEL_ERR, "Could not open and run /usr/bin/sw_vers to find macOS system version information.");
        return;
    }

    size_t line_size = CF_BUFSIZE;
    char *line = xmalloc(line_size);
    int revcomps = 0;
    unsigned int major, minor, patch;
    char *flavor = NULL, *product_name = NULL, *r;

    while (CfReadLine(&line, &line_size, pp) != -1)
    {
        if (STARTSWITH(line, "ProductName:"))
        {
            r = strrchr(line, '\t');
            if (r == NULL || ++r == NULL)
            {
                continue;
            }
            product_name = SafeStringDuplicate(r);
            ToLowerStrInplace(r);
            flavor = SafeStringDuplicate(r);
            EvalContextClassPutHard(
                    ctx,
                    r,
                    "inventory,attribute_name=none,source=agent,derived-from=sw_vers");
        }
        else if (STARTSWITH(line, "ProductVersion:"))
        {
            r = strrchr(line, '\t');
            if (r == NULL || ++r == NULL)
            {
                continue;
            }
            revcomps = sscanf(r, "%u.%u.%u", &major, &minor, &patch);
        }
    }

    if (flavor == NULL)
    {
        free(line);
        cf_pclose(pp);
        return;
    }

    char buf[CF_BUFSIZE];

    if (revcomps > 0)
    {
        NDEBUG_UNUSED int ret = snprintf(buf, sizeof(buf), "%s_%u", flavor, major);
        assert(ret >= 0 && (size_t) ret < sizeof(buf));
        Log(LOG_LEVEL_VERBOSE, "This appears to be a %s %u system.", product_name, major);
        SetFlavor(ctx, buf);
    }

    if (revcomps > 1)
    {
        NDEBUG_UNUSED int ret = snprintf(buf, sizeof(buf), "%s_%u_%u", flavor, major, minor);
        assert(ret >= 0 && (size_t) ret < sizeof(buf));
        Log(LOG_LEVEL_VERBOSE, "This appears to be a %s %u.%u system.", product_name, major, minor);
        EvalContextClassPutHard(ctx, buf, "inventory,attribute_name=none,source=agent");
    }

    if (revcomps > 2)
    {
        NDEBUG_UNUSED int ret = snprintf(buf, sizeof(buf), "%s_%u_%u_%u", flavor, major, minor, patch);
        assert(ret >= 0 && (size_t) ret < sizeof(buf));
        Log(LOG_LEVEL_VERBOSE, "This appears to be a %s %u.%u.%u system.", product_name, major, minor, patch);
        EvalContextClassPutHard(ctx, buf, "inventory,attribute_name=none,source=agent");
    }

    free(line);
    free(flavor);
    free(product_name);
    cf_pclose(pp);
}
#endif

#ifdef __linux__
static void Linux_Oracle_VM_Server_Version(EvalContext *ctx)
{
    char relstring[CF_MAXVARSIZE];
    char *r;
    int major, minor, patch;
    int revcomps;

#define ORACLE_VM_SERVER_REL_FILENAME "/etc/ovs-release"
#define ORACLE_VM_SERVER_ID "Oracle VM server"

    Log(LOG_LEVEL_VERBOSE, "This appears to be Oracle VM Server");
    EvalContextClassPutHard(ctx, "redhat", "inventory,attribute_name=none,source=agent");
    EvalContextClassPutHard(ctx, "oraclevmserver", "inventory,attribute_name=Virtual host,source=agent");

    if (!ReadLine(ORACLE_VM_SERVER_REL_FILENAME, relstring, sizeof(relstring)))
    {
        return;
    }

    if (strncmp(relstring, ORACLE_VM_SERVER_ID, strlen(ORACLE_VM_SERVER_ID)))
    {
        Log(LOG_LEVEL_VERBOSE, "Could not identify distribution from %s", ORACLE_VM_SERVER_REL_FILENAME);
        return;
    }

    if ((r = strstr(relstring, "release ")) == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "Could not find distribution version in %s", ORACLE_VM_SERVER_REL_FILENAME);
        return;
    }

    revcomps = sscanf(r + strlen("release "), "%d.%d.%d", &major, &minor, &patch);

    if (revcomps > 0)
    {
        char buf[CF_BUFSIZE];

        snprintf(buf, CF_BUFSIZE, "oraclevmserver_%d", major);
        SetFlavor(ctx, buf);
    }

    if (revcomps > 1)
    {
        char buf[CF_BUFSIZE];

        snprintf(buf, CF_BUFSIZE, "oraclevmserver_%d_%d", major, minor);
        EvalContextClassPutHard(ctx, buf, "inventory,attribute_name=none,source=agent");
    }

    if (revcomps > 2)
    {
        char buf[CF_BUFSIZE];

        snprintf(buf, CF_BUFSIZE, "oraclevmserver_%d_%d_%d", major, minor, patch);
        EvalContextClassPutHard(ctx, buf, "inventory,attribute_name=none,source=agent");
    }
}

/*********************************************************************************/

static void Linux_Oracle_Version(EvalContext *ctx)
{
    char relstring[CF_MAXVARSIZE];
    char *r;
    int major, minor;

#define ORACLE_REL_FILENAME "/etc/oracle-release"
#define ORACLE_ID "Oracle Linux Server"

    Log(LOG_LEVEL_VERBOSE, "This appears to be Oracle Linux");
    EvalContextClassPutHard(ctx, "oracle", "inventory,attribute_name=none,source=agent");

    if (!ReadLine(ORACLE_REL_FILENAME, relstring, sizeof(relstring)))
    {
        return;
    }

    if (strncmp(relstring, ORACLE_ID, strlen(ORACLE_ID)))
    {
        Log(LOG_LEVEL_VERBOSE, "Could not identify distribution from %s", ORACLE_REL_FILENAME);
        return;
    }

    if ((r = strstr(relstring, "release ")) == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "Could not find distribution version in %s", ORACLE_REL_FILENAME);
        return;
    }

    if (sscanf(r + strlen("release "), "%d.%d", &major, &minor) == 2)
    {
        char buf[CF_BUFSIZE];

        snprintf(buf, CF_BUFSIZE, "oracle_%d", major);
        SetFlavor(ctx, buf);

        snprintf(buf, CF_BUFSIZE, "oracle_%d_%d", major, minor);
        EvalContextClassPutHard(ctx, buf, "inventory,attribute_name=none,source=agent");
    }
}

/*********************************************************************************/

static int Linux_Fedora_Version(EvalContext *ctx)
{
#define FEDORA_ID "Fedora"
#define RELEASE_FLAG "release "

/* We are looking for one of the following strings...
 *
 * Fedora Core release 1 (Yarrow)
 * Fedora release 7 (Zodfoobar)
 */

#define FEDORA_REL_FILENAME "/etc/fedora-release"

/* The full string read in from fedora-release */
    char relstring[CF_MAXVARSIZE];

    Log(LOG_LEVEL_VERBOSE, "This appears to be a fedora system.");
    EvalContextClassPutHard(ctx, "redhat", "inventory,attribute_name=none,source=agent");
    EvalContextClassPutHard(ctx, "fedora", "inventory,attribute_name=none,source=agent");

/* Grab the first line from the file and then close it. */

    if (!ReadLine(FEDORA_REL_FILENAME, relstring, sizeof(relstring)))
    {
        return 1;
    }

    Log(LOG_LEVEL_VERBOSE, "Looking for fedora core linux info...");

    char *vendor = "";
    if (!strncmp(relstring, FEDORA_ID, strlen(FEDORA_ID)))
    {
        vendor = "fedora";
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Could not identify OS distro from %s", FEDORA_REL_FILENAME);
        return 2;
    }

/* Now, grok the release.  We assume that all the strings will
 * have the word 'release' before the numerical release.
 */
    int major = -1;
    char strmajor[PRINTSIZE(major)];
    char *release = strstr(relstring, RELEASE_FLAG);

    if (release == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "Could not find a numeric OS release in %s", FEDORA_REL_FILENAME);
        return 2;
    }
    else
    {
        release += strlen(RELEASE_FLAG);

        strmajor[0] = '\0';
        if (sscanf(release, "%d", &major) != 0)
        {
            xsnprintf(strmajor, sizeof(strmajor), "%d", major);
        }
    }

    if (major != -1 && vendor[0] != '\0')
    {
        char classbuf[CF_MAXVARSIZE];
        classbuf[0] = '\0';
        strcat(classbuf, vendor);
        EvalContextClassPutHard(ctx,classbuf, "inventory,attribute_name=none,source=agent");
        strcat(classbuf, "_");
        strcat(classbuf, strmajor);
        SetFlavor(ctx, classbuf);
    }

    return 0;
}

/*********************************************************************************/

static int Linux_Redhat_Version(EvalContext *ctx)
{
#define REDHAT_ID "Red Hat Linux"
#define REDHAT_ENT_ID "Red Hat Enterprise Linux"
#define REDHAT_AS_ID "Red Hat Enterprise Linux AS"
#define REDHAT_AS21_ID "Red Hat Linux Advanced Server"
#define REDHAT_ES_ID "Red Hat Enterprise Linux ES"
#define REDHAT_WS_ID "Red Hat Enterprise Linux WS"
#define REDHAT_C_ID "Red Hat Enterprise Linux Client"
#define REDHAT_S_ID "Red Hat Enterprise Linux Server"
#define REDHAT_W_ID "Red Hat Enterprise Linux Workstation"
#define REDHAT_CN_ID "Red Hat Enterprise Linux ComputeNode"
#define MANDRAKE_ID "Linux Mandrake"
#define MANDRAKE_10_1_ID "Mandrakelinux"
#define WHITEBOX_ID "White Box Enterprise Linux"
#define CENTOS_ID "CentOS"
#define SCIENTIFIC_SL_ID "Scientific Linux SL"
#define SCIENTIFIC_SL6_ID "Scientific Linux"
#define SCIENTIFIC_CERN_ID "Scientific Linux CERN"
#define RELEASE_FLAG "release "
#define ORACLE_4_5_ID "Enterprise Linux Enterprise Linux Server"

/* We are looking for one of the following strings...
 *
 * Red Hat Linux release 6.2 (Zoot)
 * Red Hat Enterprise Linux release 8.0 (Ootpa)
 * Red Hat Linux Advanced Server release 2.1AS (Pensacola)
 * Red Hat Enterprise Linux AS release 3 (Taroon)
 * Red Hat Enterprise Linux WS release 3 (Taroon)
 * Red Hat Enterprise Linux Client release 5 (Tikanga)
 * Red Hat Enterprise Linux Server release 5 (Tikanga)
 * Linux Mandrake release 7.1 (helium)
 * Red Hat Enterprise Linux ES release 2.1 (Panama)
 * White Box Enterprise linux release 3.0 (Liberation)
 * Scientific Linux SL Release 4.0 (Beryllium)
 * CentOS release 4.0 (Final)
 */

#define RH_REL_FILENAME "/etc/redhat-release"

    Log(LOG_LEVEL_VERBOSE, "This appears to be a redhat (or redhat-based) system.");
    EvalContextClassPutHard(ctx, "redhat", "inventory,attribute_name=none,source=agent,derived-from-file="RH_REL_FILENAME);

    /* Grab the first line from the file and then close it. */
    char relstring[CF_MAXVARSIZE];
    if (!ReadLine(RH_REL_FILENAME, relstring, sizeof(relstring)))
    {
        return 1;
    }

    Log(LOG_LEVEL_VERBOSE, "Looking for redhat linux info in '%s'", relstring);

    /* First, try to grok the vendor and edition (if any) */
    char *edition = ""; /* as (Advanced Server, Enterprise) */
    char *vendor = ""; /* Red Hat, Mandrake */
    if (!strncmp(relstring, REDHAT_ES_ID, strlen(REDHAT_ES_ID)))
    {
        vendor = "redhat";
        edition = "es";
    }
    else if (!strncmp(relstring, REDHAT_WS_ID, strlen(REDHAT_WS_ID)))
    {
        vendor = "redhat";
        edition = "ws";
    }
    else if (!strncmp(relstring, REDHAT_WS_ID, strlen(REDHAT_WS_ID)))
    {
        vendor = "redhat";
        edition = "ws";
    }
    else if (!strncmp(relstring, REDHAT_AS_ID, strlen(REDHAT_AS_ID)) ||
             !strncmp(relstring, REDHAT_AS21_ID, strlen(REDHAT_AS21_ID)))
    {
        vendor = "redhat";
        edition = "as";
    }
    else if (!strncmp(relstring, REDHAT_S_ID, strlen(REDHAT_S_ID)))
    {
        vendor = "redhat";
        edition = "s";
    }
    else if (!strncmp(relstring, REDHAT_C_ID, strlen(REDHAT_C_ID))
             || !strncmp(relstring, REDHAT_W_ID, strlen(REDHAT_W_ID)))
    {
        vendor = "redhat";
        edition = "c";
    }
    else if (!strncmp(relstring, REDHAT_CN_ID, strlen(REDHAT_CN_ID)))
    {
        vendor = "redhat";
        edition = "cn";
    }
    else if (!strncmp(relstring, REDHAT_ID, strlen(REDHAT_ID)))
    {
        vendor = "redhat";
    }
    else if (!strncmp(relstring, REDHAT_ENT_ID, strlen(REDHAT_ENT_ID)))
    {
        vendor = "redhat";
    }
    else if (!strncmp(relstring, MANDRAKE_ID, strlen(MANDRAKE_ID)))
    {
        vendor = "mandrake";
    }
    else if (!strncmp(relstring, MANDRAKE_10_1_ID, strlen(MANDRAKE_10_1_ID)))
    {
        vendor = "mandrake";
    }
    else if (!strncmp(relstring, WHITEBOX_ID, strlen(WHITEBOX_ID)))
    {
        vendor = "whitebox";
    }
    else if (!strncmp(relstring, SCIENTIFIC_SL_ID, strlen(SCIENTIFIC_SL_ID)))
    {
        vendor = "scientific";
        edition = "sl";
    }
    else if (!strncmp(relstring, SCIENTIFIC_CERN_ID, strlen(SCIENTIFIC_CERN_ID)))
    {
        vendor = "scientific";
        edition = "cern";
    }
    else if (!strncmp(relstring, SCIENTIFIC_SL6_ID, strlen(SCIENTIFIC_SL6_ID)))
    {
        vendor = "scientific";
        edition = "sl";
    }
    else if (!strncmp(relstring, CENTOS_ID, strlen(CENTOS_ID)))
    {
        vendor = "centos";
    }
    else if (!strncmp(relstring, ORACLE_4_5_ID, strlen(ORACLE_4_5_ID)))
    {
        vendor = "oracle";
        edition = "s";
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Could not identify OS distro from %s", RH_REL_FILENAME);
        return 2;
    }

/* Now, grok the release.  For AS, we neglect the AS at the end of the
 * numerical release because we already figured out that it *is* AS
 * from the information above.  We assume that all the strings will
 * have the word 'release' before the numerical release.
 */

/* Convert relstring to lowercase so that vendors like
   Scientific Linux don't fall through the cracks.
   */

    for (size_t i = 0; i < strlen(relstring); i++)
    {
        relstring[i] = tolower(relstring[i]);
    }

    /* Where the numerical release will be found */
    int major = -1, minor = -1;
    char strmajor[PRINTSIZE(major)], strminor[PRINTSIZE(minor)];

    char *release = strstr(relstring, RELEASE_FLAG);
    if (release == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "Could not find a numeric OS release in %s", RH_REL_FILENAME);
        return 2;
    }
    else
    {
        release += strlen(RELEASE_FLAG);
        if (sscanf(release, "%d.%d", &major, &minor) == 2)
        {
            xsnprintf(strmajor, sizeof(strmajor), "%d", major);
            xsnprintf(strminor, sizeof(strminor), "%d", minor);
        }
        /* red hat 9 is *not* red hat 9.0.
         * and same thing with RHEL AS 3
         */
        else if (sscanf(release, "%d", &major) == 1)
        {
            xsnprintf(strmajor, sizeof(strmajor), "%d", major);
            minor = -2;
        }
    }

    char classbuf[CF_MAXVARSIZE];
    if (major != -1 && minor != -1 && (strcmp(vendor, "") != 0))
    {
        classbuf[0] = '\0';
        strcat(classbuf, vendor);
        EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent,derived-from-file="RH_REL_FILENAME);
        strcat(classbuf, "_");

        if (strcmp(edition, "") != 0)
        {
            strcat(classbuf, edition);
            EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent,derived-from-file="RH_REL_FILENAME);
            strcat(classbuf, "_");
        }

        strcat(classbuf, strmajor);
        EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent,derived-from-file="RH_REL_FILENAME);

        if (minor != -2)
        {
            strcat(classbuf, "_");
            strcat(classbuf, strminor);
            EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent,derived-from-file="RH_REL_FILENAME);
        }
    }

    // Now a version without the edition
    if (major != -1 && minor != -1 && vendor[0] != '\0')
    {
        classbuf[0] = '\0';
        strcat(classbuf, vendor);
        EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent,derived-from-file="RH_REL_FILENAME);
        strcat(classbuf, "_");

        strcat(classbuf, strmajor);

        SetFlavor(ctx, classbuf);

        if (minor != -2)
        {
            strcat(classbuf, "_");
            strcat(classbuf, strminor);
            EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent,derived-from-file="RH_REL_FILENAME);
        }
    }

    return 0;
}

/******************************************************************/

static int Linux_Suse_Version(EvalContext *ctx)
{
#define SUSE_REL_FILENAME "/etc/SuSE-release"
/* Check if it's a SUSE Enterprise version (all in lowercase) */
#define SUSE_SLES8_ID "suse sles-8"
#define SUSE_SLES_ID  "suse linux enterprise server"
#define SUSE_SLED_ID  "suse linux enterprise desktop"
#define SUSE_RELEASE_FLAG "linux "

    char classbuf[CF_MAXVARSIZE];

    Log(LOG_LEVEL_VERBOSE, "This appears to be a SUSE system.");
    EvalContextClassPutHard(ctx, "SUSE", "inventory,attribute_name=none,source=agent");
    EvalContextClassPutHard(ctx, "suse", "inventory,attribute_name=none,source=agent");

    /* The correct spelling for SUSE is "SUSE" but CFEngine used to use "SuSE".
     * Keep this for backwards compatibility until CFEngine 3.7
     */
    EvalContextClassPutHard(ctx, "SuSE", "inventory,attribute_name=none,source=agent");

    /* Grab the first line from the SuSE-release file and then close it. */
    char relstring[CF_MAXVARSIZE];

    FILE *fp = ReadFirstLine(SUSE_REL_FILENAME, relstring, sizeof(relstring));
    if (fp == NULL)
    {
        return 1;
    }

    char vbuf[CF_BUFSIZE], strversion[CF_MAXVARSIZE], strpatch[CF_MAXVARSIZE];
    strversion[0] = '\0';
    strpatch[0] = '\0';

    int major = -1, minor = -1;
    while (fgets(vbuf, sizeof(vbuf), fp) != NULL)
    {
        if (strncmp(vbuf, "VERSION", strlen("version")) == 0)
        {
            strlcpy(strversion, vbuf, sizeof(strversion));
            sscanf(vbuf, "VERSION = %d", &major);
        }

        if (strncmp(vbuf, "PATCH", strlen("PATCH")) == 0)
        {
            strlcpy(strpatch, vbuf, sizeof(strpatch));
            sscanf(vbuf, "PATCHLEVEL = %d", &minor);
        }
    }
    if (ferror(fp))
    {
        UnexpectedError("Failed to read line from stream");
    }
    else
    {
        assert(feof(fp));
    }

    fclose(fp);

    /* Check if it's a SUSE Enterprise version  */

    Log(LOG_LEVEL_VERBOSE, "Looking for SUSE enterprise info in '%s'", relstring);

    /* Convert relstring to lowercase to handle rename of SuSE to
     * SUSE with SUSE 10.0.
     */

    for (size_t i = 0; i < strlen(relstring); i++)
    {
        relstring[i] = tolower(relstring[i]);
    }

    /* Check if it's a SUSE Enterprise version (all in lowercase) */

    if (!strncmp(relstring, SUSE_SLES8_ID, strlen(SUSE_SLES8_ID)))
    {
        classbuf[0] = '\0';
        strcat(classbuf, "SLES8");
        EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
    }
    else if (strncmp(relstring, "sles", 4) == 0)
    {
        Item *list, *ip;

        nt_static_assert((sizeof(vbuf)) > 255);
        sscanf(relstring, "%255[-_a-zA-Z0-9]", vbuf);
        EvalContextClassPutHard(ctx, vbuf, "inventory,attribute_name=none,source=agent");

        list = SplitString(vbuf, '-');

        for (ip = list; ip != NULL; ip = ip->next)
        {
            EvalContextClassPutHard(ctx, ip->name, "inventory,attribute_name=none,source=agent");
        }

        DeleteItemList(list);
    }
    else
    {
        for (int version = 9; version < 13; version++)
        {
            snprintf(vbuf, CF_BUFSIZE, "%s %d ", SUSE_SLES_ID, version);
            Log(LOG_LEVEL_DEBUG, "Checking for SUSE [%s]", vbuf);

            if (!strncmp(relstring, vbuf, strlen(vbuf)))
            {
                snprintf(classbuf, CF_MAXVARSIZE, "SLES%d", version);
                EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
            }
            else
            {
                snprintf(vbuf, CF_BUFSIZE, "%s %d ", SUSE_SLED_ID, version);
                Log(LOG_LEVEL_DEBUG, "Checking for SUSE [%s]", vbuf);

                if (!strncmp(relstring, vbuf, strlen(vbuf)))
                {
                    snprintf(classbuf, CF_MAXVARSIZE, "SLED%d", version);
                    EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
                }
            }
        }
    }

    /* Determine release version. We assume that the version follows
     * the string "SuSE Linux" or "SUSE LINUX".
     */

    char *release = strstr(relstring, SUSE_RELEASE_FLAG);
    if (release == NULL)
    {
        release = strstr(relstring, "opensuse");
        if (release == NULL)
        {
            release = strversion;
        }
    }

    if (release == NULL)
    {
        Log(LOG_LEVEL_VERBOSE,
            "Could not find a numeric OS release in %s",
            SUSE_REL_FILENAME);
        return 2;
    }
    else
    {
        char strmajor[CF_MAXVARSIZE], strminor[CF_MAXVARSIZE];
        if (strchr(release, '.'))
        {
            sscanf(release, "%*s %d.%d", &major, &minor);
            xsnprintf(strmajor, sizeof(strmajor), "%d", major);
            xsnprintf(strminor, sizeof(strminor), "%d", minor);

            if (major != -1 && minor != -1)
            {
                strcpy(classbuf, "SUSE");
                EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
                strcat(classbuf, "_");
                strcat(classbuf, strmajor);
                SetFlavor(ctx, classbuf);
                strcat(classbuf, "_");
                strcat(classbuf, strminor);
                EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");

                /* The correct spelling for SUSE is "SUSE" but CFEngine used to use "SuSE".
                 * Keep this for backwards compatibility until CFEngine 3.7
                 */
                strcpy(classbuf, "SuSE");
                EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
                strcat(classbuf, "_");
                strcat(classbuf, strmajor);
                EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
                strcat(classbuf, "_");
                strcat(classbuf, strminor);
                EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");

                Log(LOG_LEVEL_VERBOSE, "Discovered SUSE version %s", classbuf);
                return 0;
            }
        }
        else
        {
            nt_static_assert((sizeof(strmajor)) > 255);
            nt_static_assert((sizeof(strminor)) > 255);
            sscanf(strversion, "VERSION = %255s", strmajor);
            sscanf(strpatch, "PATCHLEVEL = %255s", strminor);

            if (major != -1 && minor != -1)
            {
                strcpy(classbuf, "SLES");
                EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
                strcat(classbuf, "_");
                strcat(classbuf, strmajor);
                EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
                strcat(classbuf, "_");
                strcat(classbuf, strminor);
                EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");

                snprintf(classbuf, CF_MAXVARSIZE, "SUSE_%d", major);
                SetFlavor(ctx, classbuf);

                /* The correct spelling for SUSE is "SUSE" but CFEngine used to use "SuSE".
                 * Keep this for backwards compatibility until CFEngine 3.7
                 */
                snprintf(classbuf, CF_MAXVARSIZE, "SuSE_%d", major);
                EvalContextClassPutHard(ctx, classbuf, "source=agent");

                Log(LOG_LEVEL_VERBOSE, "Discovered SUSE version %s", classbuf);
                return 0;
            }
        }
    }

    Log(LOG_LEVEL_VERBOSE, "Could not find a numeric OS release in %s", SUSE_REL_FILENAME);

    return 0;
}

/******************************************************************/

static int Linux_Slackware_Version(EvalContext *ctx, char *filename)
{
    int major = -1;
    int minor = -1;
    int release = -1;
    char classname[CF_MAXVARSIZE] = "";
    char buffer[CF_MAXVARSIZE];

    Log(LOG_LEVEL_VERBOSE, "This appears to be a slackware system.");
    EvalContextClassPutHard(ctx, "slackware", "inventory,attribute_name=none,source=agent");

    if (!ReadLine(filename, buffer, sizeof(buffer)))
    {
        return 1;
    }

    Log(LOG_LEVEL_VERBOSE, "Looking for Slackware version...");
    switch (sscanf(buffer, "Slackware %d.%d.%d", &major, &minor, &release))
    {
    case 3:
        Log(LOG_LEVEL_VERBOSE, "This appears to be a Slackware %u.%u.%u system.", major, minor, release);
        snprintf(classname, CF_MAXVARSIZE, "slackware_%u_%u_%u", major, minor, release);
        EvalContextClassPutHard(ctx, classname, "inventory,attribute_name=none,source=agent");
        /* Fall-through */
    case 2:
        Log(LOG_LEVEL_VERBOSE, "This appears to be a Slackware %u.%u system.", major, minor);
        snprintf(classname, CF_MAXVARSIZE, "slackware_%u_%u", major, minor);
        EvalContextClassPutHard(ctx, classname, "inventory,attribute_name=none,source=agent");
        /* Fall-through */
    case 1:
        Log(LOG_LEVEL_VERBOSE, "This appears to be a Slackware %u system.", major);
        snprintf(classname, CF_MAXVARSIZE, "slackware_%u", major);
        EvalContextClassPutHard(ctx, classname, "inventory,attribute_name=none,source=agent");
        break;
    case 0:
        Log(LOG_LEVEL_VERBOSE, "No Slackware version number found.");
        return 2;
    }
    return 0;
}

/*
 * @brief Purge /etc/issue escapes on debian
 *
 * On debian, /etc/issue can include special characters escaped with
 * '\\' or '@'. This function removes such escape sequences.
 *
 * @param[in,out] buffer: string to be sanitized
 */
static void LinuxDebianSanitizeIssue(char *buffer)
{
    bool escaped = false;
    char *dst = buffer, *src = buffer, *tail = dst;
    while (*src != '\0')
    {
        char here = *src;
        src++;
        if (here == '\\' || here == '@' || escaped)
        {
            /* Skip over escapes and the character each acts on. */
            escaped = !escaped;
        }
        else
        {
            /* Copy everything else verbatim: */
            *dst = here;
            dst++;
            /* Keep track of (just after) last non-space: */
            if (!isspace(here))
            {
                tail = dst;
            }
        }
    }

    assert(tail == dst || isspace(*tail));
    *tail = '\0';
}

/******************************************************************/

static int Linux_Misc_Version(EvalContext *ctx)
{
    char flavor[CF_MAXVARSIZE];
    char version[256];
    char buffer[CF_BUFSIZE];

    const char *os = NULL;
    version[0] = '\0';

    FILE *fp = safe_fopen(LSB_RELEASE_FILENAME, "r");
    if (fp != NULL)
    {
        while (!feof(fp))
        {
            if (fgets(buffer, CF_BUFSIZE, fp) == NULL)
            {
                if (ferror(fp))
                {
                    break;
                }
                continue;
            }

            if (strstr(buffer, "Cumulus"))
            {
                EvalContextClassPutHard(ctx, "cumulus", "inventory,attribute_name=none,source=agent");
                os = "cumulus";
            }

            char *sp = strstr(buffer, "DISTRIB_RELEASE=");
            if (sp)
            {
                version[0] = '\0';
                nt_static_assert((sizeof(version)) > 255);
                sscanf(sp + strlen("DISTRIB_RELEASE="), "%255[^\n]", version);
                CanonifyNameInPlace(version);
            }
        }
        fclose(fp);
    }

    if (os != NULL && version[0] != '\0')
    {
        snprintf(flavor, CF_MAXVARSIZE, "%s_%s", os, version);
        SetFlavor(ctx, flavor);
        return 1;
    }

    return 0;
}

/******************************************************************/

static int Linux_Debian_Version(EvalContext *ctx)
{
    int major = -1;
    int release = -1;
    int result;
    char classname[CF_MAXVARSIZE], buffer[CF_MAXVARSIZE], os[CF_MAXVARSIZE];
    char version[256]; // Size must agree with sscanfs below

    Log(LOG_LEVEL_VERBOSE, "This appears to be a debian system.");
    EvalContextClassPutHard(
        ctx,
        "debian",
        "inventory,attribute_name=none,source=agent,derived-from-file="DEBIAN_VERSION_FILENAME);

    buffer[0] = classname[0] = '\0';

    Log(LOG_LEVEL_VERBOSE, "Looking for Debian version...");

    if (!ReadLine(DEBIAN_VERSION_FILENAME, buffer, sizeof(buffer)))
    {
        return 1;
    }

    result = sscanf(buffer, "%d.%d", &major, &release);

    switch (result)
    {
    case 2:
        Log(LOG_LEVEL_VERBOSE, "This appears to be a Debian %u.%u system.", major, release);
        snprintf(classname, CF_MAXVARSIZE, "debian_%u_%u", major, release);
        EvalContextClassPutHard(
            ctx,
            classname,
            "inventory,attribute_name=none,source=agent,derived-from-file="DEBIAN_VERSION_FILENAME);
        snprintf(classname, CF_MAXVARSIZE, "debian_%u", major);
        SetFlavor(ctx, classname);
        break;

    case 1:
        Log(LOG_LEVEL_VERBOSE, "This appears to be a Debian %u system.", major);
        snprintf(classname, CF_MAXVARSIZE, "debian_%u", major);
        SetFlavor(ctx, classname);
        break;

    default:
        version[0] = '\0';
        nt_static_assert((sizeof(version)) > 25);
        sscanf(buffer, "%25[^/]", version);
        if (strlen(version) > 0)
        {
            snprintf(classname, CF_MAXVARSIZE, "debian_%s", version);
            EvalContextClassPutHard(
                ctx,
                classname,
                "inventory,attribute_name=none,source=agent,derived-from-file="DEBIAN_VERSION_FILENAME);
        }
        break;
    }

    if (!ReadLine(DEBIAN_ISSUE_FILENAME, buffer, sizeof(buffer)))
    {
        return 1;
    }

    os[0] = '\0';
    nt_static_assert((sizeof(os)) > 250);
    sscanf(buffer, "%250s", os);

    if (strcmp(os, "Debian") == 0)
    {
        LinuxDebianSanitizeIssue(buffer);
        nt_static_assert((sizeof(version)) > 255);
        sscanf(buffer, "%*s %*s %255[^./]", version);
        snprintf(buffer, CF_MAXVARSIZE, "debian_%s", version);
        EvalContextClassPutHard(
            ctx,
            "debian",
            "inventory,attribute_name=none,source=agent,derived-from-file="DEBIAN_ISSUE_FILENAME);
        SetFlavor(ctx, buffer);
    }
    else if (strcmp(os, "Ubuntu") == 0)
    {
        LinuxDebianSanitizeIssue(buffer);
        char minor[256] = {0};
        nt_static_assert((sizeof(version)) > 255);
        sscanf(buffer, "%*s %255[^.].%255s", version, minor);
        snprintf(buffer, CF_MAXVARSIZE, "ubuntu_%s", version);
        SetFlavor(ctx, buffer);
        EvalContextClassPutHard(
            ctx,
            "ubuntu",
            "inventory,attribute_name=none,source=agent,derived-from-file="DEBIAN_ISSUE_FILENAME);
        if (release >= 0)
        {
            snprintf(buffer, CF_MAXVARSIZE, "ubuntu_%s_%s", version, minor);
            EvalContextClassPutHard(
                ctx,
                buffer,
                "inventory,attribute_name=none,source=agent,derived-from-file="DEBIAN_ISSUE_FILENAME);
        }
    }

    return 0;
}

/******************************************************************/

static int Linux_Mandrake_Version(EvalContext *ctx)
{
/* We are looking for one of the following strings... */
#define MANDRAKE_ID "Linux Mandrake"
#define MANDRAKE_REV_ID "Mandrake Linux"
#define MANDRAKE_10_1_ID "Mandrakelinux"

#define MANDRAKE_REL_FILENAME "/etc/mandrake-release"

    char relstring[CF_MAXVARSIZE];
    char *vendor = NULL;

    Log(LOG_LEVEL_VERBOSE, "This appears to be a mandrake system.");
    EvalContextClassPutHard(ctx, "Mandrake", "inventory,attribute_name=none,source=agent");

    if (!ReadLine(MANDRAKE_REL_FILENAME, relstring, sizeof(relstring)))
    {
        return 1;
    }

    Log(LOG_LEVEL_VERBOSE, "Looking for Mandrake linux info in '%s'", relstring);

/* Older Mandrakes had the 'Mandrake Linux' string in reverse order */

    if (!strncmp(relstring, MANDRAKE_ID, strlen(MANDRAKE_ID)))
    {
        vendor = "mandrake";
    }
    else if (!strncmp(relstring, MANDRAKE_REV_ID, strlen(MANDRAKE_REV_ID)))
    {
        vendor = "mandrake";
    }

    else if (!strncmp(relstring, MANDRAKE_10_1_ID, strlen(MANDRAKE_10_1_ID)))
    {
        vendor = "mandrake";
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Could not identify OS distro from %s", MANDRAKE_REL_FILENAME);
        return 2;
    }

    return Linux_Mandriva_Version_Real(ctx, MANDRAKE_REL_FILENAME, relstring, vendor);
}

/******************************************************************/

static int Linux_Mandriva_Version(EvalContext *ctx)
{
/* We are looking for the following strings... */
#define MANDRIVA_ID "Mandriva Linux"

#define MANDRIVA_REL_FILENAME "/etc/mandriva-release"

    char relstring[CF_MAXVARSIZE];
    char *vendor = NULL;

    Log(LOG_LEVEL_VERBOSE, "This appears to be a mandriva system.");
    EvalContextClassPutHard(ctx, "Mandrake", "inventory,attribute_name=none,source=agent");
    EvalContextClassPutHard(ctx, "Mandriva", "inventory,attribute_name=none,source=agent");

    if (!ReadLine(MANDRIVA_REL_FILENAME, relstring, sizeof(relstring)))
    {
        return 1;
    }

    Log(LOG_LEVEL_VERBOSE, "Looking for Mandriva linux info in '%s'", relstring);

    if (!strncmp(relstring, MANDRIVA_ID, strlen(MANDRIVA_ID)))
    {
        vendor = "mandriva";
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Could not identify OS distro from '%s'", MANDRIVA_REL_FILENAME);
        return 2;
    }

    return Linux_Mandriva_Version_Real(ctx, MANDRIVA_REL_FILENAME, relstring, vendor);

}

/******************************************************************/

static int Linux_Mandriva_Version_Real(EvalContext *ctx, char *filename, char *relstring, char *vendor)
{
    int major = -1, minor = -1;
    char strmajor[PRINTSIZE(major)], strminor[PRINTSIZE(minor)];

#define RELEASE_FLAG "release "
    char *release = strstr(relstring, RELEASE_FLAG);
    if (release == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "Could not find a numeric OS release in %s", filename);
        return 2;
    }
    else
    {
        release += strlen(RELEASE_FLAG);
        if (sscanf(release, "%d.%d", &major, &minor) == 2)
        {
            xsnprintf(strmajor, sizeof(strmajor), "%d", major);
            xsnprintf(strminor, sizeof(strminor), "%d", minor);
        }
        else
        {
            Log(LOG_LEVEL_VERBOSE, "Could not break down release version numbers in %s", filename);
        }
    }

    if (major != -1 && minor != -1 && strcmp(vendor, ""))
    {
        char classbuf[CF_MAXVARSIZE];
        classbuf[0] = '\0';
        strcat(classbuf, vendor);
        EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
        strcat(classbuf, "_");
        strcat(classbuf, strmajor);
        EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
        if (minor != -2)
        {
            strcat(classbuf, "_");
            strcat(classbuf, strminor);
            EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
        }
    }

    return 0;
}

/******************************************************************/

static void Linux_Amazon_Version(EvalContext *ctx)
{
    char buffer[CF_BUFSIZE];

    // Amazon Linux release 2 (Karoo)

    if (ReadLine("/etc/system-release", buffer, sizeof(buffer)))
    {
        if (strstr(buffer, "Amazon") != NULL)
        {
            EvalContextClassPutHard(ctx, "amazon_linux",
                "inventory,attribute_name=none,source=agent"
                ",derived-from-file=/etc/system-release");

            char version[128];
            if (sscanf(buffer, "%*s %*s %*s %127s", version) == 1)
            {
                char class[CF_MAXVARSIZE];

                CanonifyNameInPlace(version);
                snprintf(class, sizeof(class), "amazon_linux_%s", version);
                EvalContextClassPutHard(ctx, class,
                    "inventory,attribute_name=none,source=agent"
                    ",derived-from-file=/etc/system-release");
                SetFlavor(ctx, class);
            }
            else
            {
                SetFlavor(ctx, "amazon_linux");
            }
            // We set this class for backwards compatibility
            EvalContextClassPutHard(ctx, "AmazonLinux",
                    "inventory,attribute_name=none,source=agent,"
                    "derived-from=sys.flavor");
        }
    }
}

/******************************************************************/

static void Linux_Alpine_Version(EvalContext *ctx)
{
    char buffer[CF_BUFSIZE];

    Log(LOG_LEVEL_VERBOSE, "This appears to be an AlpineLinux system.");

    EvalContextClassPutHard(ctx, "alpine_linux",
        "inventory,attribute_name=none,source=agent"
        ",derived-from-file=/etc/alpine-release");

    if (ReadLine("/etc/alpine-release", buffer, sizeof(buffer)))
    {
        char version[128];
        if (sscanf(buffer, "%127s", version) == 1)
        {
            char class[CF_MAXVARSIZE];
            CanonifyNameInPlace(version);
            snprintf(class, sizeof(class), "alpine_linux_%s", version);
            EvalContextClassPutHard(ctx, class,
                "inventory,attribute_name=none,source=agent"
                ",derived-from-file=/etc/alpine-release");
        }
    }
    SetFlavor(ctx, "alpinelinux");
}

/******************************************************************/

static int EOS_Version(EvalContext *ctx)

{
    char buffer[CF_BUFSIZE];

 // e.g. Arista Networks EOS 4.10.2

    if (ReadLine("/etc/Eos-release", buffer, sizeof(buffer)))
    {
        if (strstr(buffer, "EOS"))
        {
            char version[256], class[CF_MAXVARSIZE];
            EvalContextClassPutHard(ctx, "eos", "inventory,attribute_name=none,source=agent");
            EvalContextClassPutHard(ctx, "arista", "source=agent");
            version[0] = '\0';
            sscanf(buffer, "%*s %*s %*s %255s", version);
            CanonifyNameInPlace(version);
            snprintf(class, CF_MAXVARSIZE, "eos_%s", version);
            EvalContextClassPutHard(ctx, class, "inventory,attribute_name=none,source=agent");
        }
    }

    return 0;
}

/******************************************************************/

static int MiscOS(EvalContext *ctx)

{ char buffer[CF_BUFSIZE];

 // e.g. BIG-IP 10.1.0 Build 3341.1084

    if (ReadLine("/etc/issue", buffer, sizeof(buffer)))
    {
       if (strstr(buffer, "BIG-IP"))
       {
           char version[256], build[256], class[CF_MAXVARSIZE];
           EvalContextClassPutHard(ctx, "big_ip", "inventory,attribute_name=none,source=agent");
           sscanf(buffer, "%*s %255s %*s %255s", version, build);
           CanonifyNameInPlace(version);
           CanonifyNameInPlace(build);
           snprintf(class, CF_MAXVARSIZE, "big_ip_%s", version);
           EvalContextClassPutHard(ctx, class, "inventory,attribute_name=none,source=agent");
           snprintf(class, CF_MAXVARSIZE, "big_ip_%s_%s", version, build);
           EvalContextClassPutHard(ctx, class, "inventory,attribute_name=none,source=agent");
           SetFlavor(ctx, "BIG-IP");
       }
    }

    return 0;
}

/******************************************************************/

static int VM_Version(EvalContext *ctx)
{
    char *sp, buffer[CF_BUFSIZE], classbuf[CF_BUFSIZE], version[256];
    int major, minor, bug;
    int sufficient = 0;

    Log(LOG_LEVEL_VERBOSE, "This appears to be a VMware Server ESX/xSX system.");
    EvalContextClassPutHard(ctx, "VMware", "inventory,attribute_name=Virtual host,source=agent");

/* VMware Server ESX >= 3 has version info in /proc */
    if (ReadLine("/proc/vmware/version", buffer, sizeof(buffer)))
    {
        if (sscanf(buffer, "VMware ESX Server %d.%d.%d", &major, &minor, &bug) > 0)
        {
            snprintf(classbuf, CF_BUFSIZE, "VMware ESX Server %d", major);
            EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
            snprintf(classbuf, CF_BUFSIZE, "VMware ESX Server %d.%d", major, minor);
            EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
            snprintf(classbuf, CF_BUFSIZE, "VMware ESX Server %d.%d.%d", major, minor, bug);
            EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
            sufficient = 1;
        }
        else if (sscanf(buffer, "VMware ESX Server %255s", version) > 0)
        {
            snprintf(classbuf, CF_BUFSIZE, "VMware ESX Server %s", version);
            EvalContextClassPutHard(ctx, classbuf, "inventory,attribute_name=none,source=agent");
            sufficient = 1;
        }
    }

/* Fall back to checking for other files */

    if (sufficient < 1 && (ReadLine("/etc/vmware-release", buffer, sizeof(buffer))
                           || ReadLine("/etc/issue", buffer, sizeof(buffer))))
    {
        EvalContextClassPutHard(ctx, buffer, "inventory,attribute_name=none,source=agent");

        /* Strip off the release code name e.g. "(Dali)" */
        if ((sp = strchr(buffer, '(')) != NULL)
        {
            *sp = 0;
            Chop(buffer, CF_EXPANDSIZE);
            EvalContextClassPutHard(ctx, buffer, "inventory,attribute_name=none,source=agent");
        }
        sufficient = 1;
    }

    return sufficient < 1 ? 1 : 0;
}

/******************************************************************/

static int Xen_Domain(EvalContext *ctx)
{
    int sufficient = 0;

    Log(LOG_LEVEL_VERBOSE, "This appears to be a xen pv system.");
    EvalContextClassPutHard(ctx, "xen", "inventory,attribute_name=Virtual host,source=agent");

/* xen host will have "control_d" in /proc/xen/capabilities, xen guest will not */

    FILE *fp = safe_fopen("/proc/xen/capabilities", "r");
    if (fp != NULL)
    {
        size_t buffer_size = CF_BUFSIZE;
        char *buffer = xmalloc(buffer_size);

        for (;;)
        {
            ssize_t res = CfReadLine(&buffer, &buffer_size, fp);
            if (res == -1)
            {
                if (!feof(fp))
                {
                    /* Failure reading Xen capabilites. Do we care? */
                    fclose(fp);
                    free(buffer);
                    return 1;
                }
                else
                {
                    break;
                }
            }

            if (strstr(buffer, "control_d"))
            {
                EvalContextClassPutHard(ctx, "xen_dom0", "inventory,attribute_name=Virtual host,source=agent");
                sufficient = 1;
            }
        }

        if (!sufficient)
        {
            EvalContextClassPutHard(ctx, "xen_domu_pv", "inventory,attribute_name=Virtual host,source=agent");
            sufficient = 1;
        }

        fclose(fp);
        free(buffer);
    }

    return sufficient < 1 ? 1 : 0;
}

/******************************************************************/
static void OpenVZ_Detect(EvalContext *ctx)
{
/* paths to file defining the type of vm (guest or host) */
#define OPENVZ_HOST_FILENAME "/proc/bc/0"
#define OPENVZ_GUEST_FILENAME "/proc/vz"
/* path to the vzps binary */
#define OPENVZ_VZPS_FILE "/bin/vzps"
    struct stat statbuf;

    /* The file /proc/bc/0 is present on host
       The file /proc/vz is present on guest
       If the host has /bin/vzps, we should use it for checking processes
    */

    if (stat(OPENVZ_HOST_FILENAME, &statbuf) != -1)
    {
        Log(LOG_LEVEL_VERBOSE, "This appears to be an OpenVZ/Virtuozzo/Parallels Cloud Server host system.\n");
        EvalContextClassPutHard(ctx, "virt_host_vz", "inventory,attribute_name=Virtual host,source=agent");
        /* if the file /bin/vzps is there, it is safe to use the processes promise type */
        if (stat(OPENVZ_VZPS_FILE, &statbuf) != -1)
        {
            EvalContextClassPutHard(ctx, "virt_host_vz_vzps", "inventory,attribute_name=Virtual host,source=agent");
            /* here we must redefine the value of VPSHARDCLASS */
            for (int i = 0; i < PLATFORM_CONTEXT_MAX; i++)
            {
                if (!strcmp(CLASSATTRIBUTES[i][0], "virt_host_vz_vzps"))
                {
                   VPSHARDCLASS = (PlatformContext) i;
                   break;
                }
            }
        }
        else
        {
            Log(LOG_LEVEL_NOTICE, "This OpenVZ/Virtuozzo/Parallels Cloud Server host system does not have vzps installed; the processes promise type may not work as expected.\n");
        }
    }
    else if (stat(OPENVZ_GUEST_FILENAME, &statbuf) != -1)
    {
        Log(LOG_LEVEL_VERBOSE, "This appears to be an OpenVZ/Virtuozzo/Parallels Cloud Server guest system.\n");
        EvalContextClassPutHard(ctx, "virt_guest_vz", "inventory,attribute_name=Virtual host,source=agent");
    }
}

/******************************************************************/

static bool ReadLine(const char *filename, char *buf, int bufsize)
{
    FILE *fp = ReadFirstLine(filename, buf, bufsize);

    if (fp == NULL)
    {
        return false;
    }
    else
    {
        fclose(fp);
        return true;
    }
}

static FILE *ReadFirstLine(const char *filename, char *buf, int bufsize)
{
    FILE *fp = safe_fopen(filename, "r");

    if (fp == NULL)
    {
        return NULL;
    }

    if (fgets(buf, bufsize, fp) == NULL)
    {
        fclose(fp);
        return NULL;
    }

    StripTrailingNewline(buf, CF_EXPANDSIZE);

    return fp;
}
#endif /* __linux__ */

/******************************************************************/

#ifdef XEN_CPUID_SUPPORT

/* Borrowed and modified from Xen source/tools/libxc/xc_cpuid_x86.c */

static void Xen_Cpuid(uint32_t idx, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
# ifdef __i386__
    /* On i386, %ebx register needs to be saved before usage and restored
     * thereafter for PIC-compliant code on i386. */
    asm("push %%ebx; cpuid; mov %%ebx,%1; pop %%ebx"
        : "=a"(*eax), "=r"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "0" (idx),  "2" (0) );
# else
    asm("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "0" (idx),  "2" (0) );
# endif
}

/******************************************************************/

static bool Xen_Hv_Check(void)
{
    /* CPUID interface to Xen from arch-x86/cpuid.h:
     * Leaf 1 (0x40000000)
     * EAX: Largest Xen-information leaf. All leaves up to an including @EAX
     *   are supported by the Xen host.
     * EBX-EDX: "XenVMMXenVMM" signature, allowing positive identification
     *   of a Xen host.
     *
     * Additional information can be found in the Hypervisor CPUID
     * Interface Proposal (https://lkml.org/lkml/2008/10/1/246)
     */

    uint32_t eax, base;
    union
    {
        uint32_t u[3];
        char     s[13];
    } sig = {{0}};

    /*
     * For compatibility with other hypervisor interfaces, the Xen cpuid leaves
     * can be found at the first otherwise unused 0x100 aligned boundary starting
     * from 0x40000000.
     *
     * e.g If viridian extensions are enabled for an HVM domain, the Xen cpuid
     * leaves will start at 0x40000100
     */
    for (base = 0x40000000; base < 0x40010000; base += 0x100)
    {
        Xen_Cpuid(base, &eax, &sig.u[0], &sig.u[1], &sig.u[2]);

        if (memcmp("XenVMMXenVMM", &sig.s[0], 12) == 0)
        {
            /* The highest basic calling parameter (largest value that EAX can
             * be set to before calling CPUID) is returned in EAX. */
            if ((eax - base) < 2)
            {
                Log(LOG_LEVEL_DEBUG,
                    "Insufficient Xen CPUID Leaves (eax=%x at base %x)",
                    eax, base);
                return false;
            }
            Log(LOG_LEVEL_DEBUG,
                "Found Xen CPUID Leaf (eax=%x at base %x)", eax, base);
            return true;
        }
    }

    return false;
}

#endif /* XEN_CPUID_SUPPORT */

static void GetCPUInfo(EvalContext *ctx)
{
#if defined(MINGW) || defined(NT)
    Log(LOG_LEVEL_VERBOSE, "!! cpu count not implemented on Windows platform");
    return;
#else
    int count = 0;

    // http://preview.tinyurl.com/c9l2sh - StackOverflow on cross-platform CPU counting
#if defined(HAVE_SYSCONF) && defined(_SC_NPROCESSORS_ONLN)
    // Linux, AIX, Solaris, Darwin >= 10.4
    count = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif

#ifdef __linux__
    int package_id = 0, max_package_id = 0;
    char buffer[CF_SMALLBUF] = "";
    StringSet *package_id_files = GlobFileList("/sys/devices/system/cpu/cpu*/topology/physical_package_id");
    StringSetIterator it = StringSetIteratorInit(package_id_files);
    const char *file = NULL;

    while ((file = StringSetIteratorNext(&it)))
    {
        int f = open(file, O_RDONLY);
        if (f == -1)
        {
            continue;
        }
        ssize_t n_read = FullRead(f, buffer, sizeof(buffer));
        close(f);
        if (n_read < 1)
        {
            continue;
        }
        if (sscanf(buffer, "%d", &package_id) == 1)
        {
            if (package_id > max_package_id)
            {
                max_package_id = package_id;
            }
        }
    }
    max_package_id++;
    StringSetDestroy(package_id_files);

    if (max_package_id == 1)
    {
        NDEBUG_UNUSED int ret = snprintf(buffer, CF_SMALLBUF, "%d_cpusocket", max_package_id);
        assert(ret >= 0 && ret < CF_SMALLBUF);
    }
    else
    {
        NDEBUG_UNUSED int ret = snprintf(buffer, CF_SMALLBUF, "%d_cpusockets", max_package_id);
        assert(ret >= 0 && ret < CF_SMALLBUF);
    }
    EvalContextClassPutHard(ctx, buffer, "source=agent,derived-from=sys.cpusockets");
    NDEBUG_UNUSED int ret = snprintf(buffer, CF_SMALLBUF, "%d", max_package_id);
    assert(ret >= 0 && ret < CF_SMALLBUF);
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cpusockets", buffer, CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=CPU sockets");
#endif

#if defined(HAVE_SYS_SYSCTL_H) && defined(HW_NCPU)
    // BSD-derived platforms
    int mib[2] = { CTL_HW, HW_NCPU };
    size_t len;

    len = sizeof(count);
    if(sysctl(mib, 2, &count, &len, NULL, 0) < 0)
    {
        Log(LOG_LEVEL_ERR, "!! failed to get cpu count: %s", strerror(errno));
    }
#endif

#ifdef HAVE_SYS_MPCTL_H
// Itanium processors have Intel Hyper-Threading virtual-core capability,
// and the MPC_GETNUMCORES_SYS operation counts each HT virtual core,
// which is equivalent to what the /proc/stat scan delivers for Linux.
//
// A build on 11i v3 PA would have the GETNUMCORES define, but if run on an
// 11i v1 system it would fail since that OS release has only GETNUMSPUS.
// So in the presence of GETNUMCORES, we check for an invalid arg error
// and fall back to GETNUMSPUS if necessary. An 11i v1 build would work
// normally on 11i v3, because on PA-RISC cores == spus since there's no
// HT on PA-RISC, and 11i v1 only runs on PA-RISC.
# ifdef MPC_GETNUMCORES_SYS
    count = mpctl(MPC_GETNUMCORES_SYS, 0, 0);
    if (count == -1 && errno == EINVAL)
    {
        count = mpctl(MPC_GETNUMSPUS_SYS, 0, 0);
    }
# else
    count = mpctl(MPC_GETNUMSPUS_SYS, 0, 0);	// PA-RISC processor count
# endif
#endif /* HAVE_SYS_MPCTL_H */

    if (count < 1)
    {
        Log(LOG_LEVEL_VERBOSE, "invalid processor count: %d", count);
        return;
    }
    Log(LOG_LEVEL_VERBOSE, "Found %d processor%s", count, count > 1 ? "s" : "");

    char buf[CF_SMALLBUF] = "1_cpu";
    if (count == 1)
    {
        EvalContextClassPutHard(ctx, buf, "source=agent,derived-from=sys.cpus");  // "1_cpu" from init - change if buf is ever used above
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cpus", "1", CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=CPU logical cores");
    }
    else
    {
        snprintf(buf, CF_SMALLBUF, "%d_cpus", count);
        EvalContextClassPutHard(ctx, buf, "source=agent,derived-from=sys.cpus");
        snprintf(buf, CF_SMALLBUF, "%d", count);
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "cpus", buf, CF_DATA_TYPE_STRING, "inventory,source=agent,attribute_name=CPU logical cores");
    }
#endif /* MINGW || NT */
}

/******************************************************************/

// Implemented in Windows specific section.
#ifndef __MINGW32__

/**
   Return the number of seconds the system has been online given the current
   time() as an argument, or return -1 if unavailable or unimplemented.
*/
int GetUptimeSeconds(time_t now)
{
    time_t boot_time = 0;
    errno = 0;

#if defined(BOOT_TIME_WITH_SYSINFO)         // Most GNU, Linux platforms

    struct sysinfo s;
    if (sysinfo(&s) == 0)
    {
       // Don't return yet, sanity checking below
       boot_time = now - s.uptime;
    }

#elif defined(BOOT_TIME_WITH_KSTAT)         // Solaris platform

    /* From command line you can get this with:
       kstat -p unix:0:system_misc:boot_time */
    kstat_ctl_t *kc = kstat_open();
    if(kc != 0)
    {
        kstat_t *kp = kstat_lookup(kc, "unix", 0, "system_misc");
        if(kp != 0)
        {
            if (kstat_read(kc, kp, NULL) != -1)
            {
                kstat_named_t *knp = kstat_data_lookup(kp, "boot_time");
                if (knp != NULL)
                {
                    boot_time = knp->value.ui32;
                }
            }
        }
        kstat_close(kc);
    }

#elif defined(BOOT_TIME_WITH_PSTAT_GETPROC) // HP-UX platform only

    struct pst_status p;
    if (pstat_getproc(&p, sizeof(p), 0, 1) == 1)
    {
        boot_time = (time_t)p.pst_start;
    }

#elif defined(BOOT_TIME_WITH_SYSCTL)        // BSD-derived platforms

    int mib[2] = { CTL_KERN, KERN_BOOTTIME };
    struct timeval boot;
    size_t len = sizeof(boot);
    if (sysctl(mib, 2, &boot, &len, NULL, 0) == 0)
    {
        boot_time = boot.tv_sec;
    }

#elif defined(BOOT_TIME_WITH_PROCFS)

    struct stat p;
    if (stat("/proc/1", &p) == 0)
    {
        boot_time = p.st_ctime;
    }

#elif defined(BOOT_TIME_WITH_UTMP)          /* SystemV, highly portable way */

    struct utmp query = { .ut_type = BOOT_TIME };
    struct utmp *result;
    setutent();
    result = getutid(&query);
    if (result != NULL)
    {
        boot_time = result->ut_time;
    }
    endutent();

#elif defined(BOOT_TIME_WITH_UTMPX)                            /* POSIX way */

    struct utmpx query = { .ut_type = BOOT_TIME };
    struct utmpx *result;
    setutxent();
    result = getutxid(&query);
    if (result != NULL)
    {
        boot_time = result->ut_tv.tv_sec;
    }
    endutxent();

#endif

    if(errno)
    {
        Log(LOG_LEVEL_VERBOSE, "boot time discovery error: %s", GetErrorStr());
    }

    if(boot_time > now || boot_time <= 0)
    {
        Log(LOG_LEVEL_VERBOSE, "invalid boot time found; trying uptime command");
        boot_time = GetBootTimeFromUptimeCommand(now);
    }

    return boot_time > 0 ? now - boot_time : -1;
}
#endif // !__MINGW32__

int GetUptimeMinutes(time_t now)
{
    return GetUptimeSeconds(now) / SECONDS_PER_MINUTE;
}

/******************************************************************/

// Last resort: parse the output of the uptime command with a PCRE regexp
// and convert the uptime to boot time using "now" argument.
//
// The regexp needs to match all variants of the uptime command's output.
// Solaris 8/9/10:  10:45am up 109 day(s), 19:56, 1 user, load average:
// HP-UX 11.11:   9:24am  up 1 min,  1 user,  load average:
//                8:23am  up 23 hrs,  0 users,  load average:
//                9:33am  up 2 days, 10 mins,  0 users,  load average:
//                11:23am  up 2 days, 2 hrs,  0 users,  load average:
// Red Hat Linux: 10:51:23 up 5 days, 19:54, 1 user, load average:
//
// UPTIME_BACKREFS must be set to this regexp's maximum backreference
// index number (i.e., the count of left-parentheses):
#define UPTIME_REGEXP " up (\\d+ day[^,]*,|) *(\\d+( ho?u?r|:(\\d+))|(\\d+) min)"
#define UPTIME_BACKREFS 5

static time_t GetBootTimeFromUptimeCommand(time_t now)
{
    FILE *uptimecmd;
    char *backref = NULL;
    const char *uptimepath = "/usr/bin/uptime";
    time_t uptime = 0;

    int err_code;
    size_t err_offset;
    pcre2_code *regex = pcre2_compile((PCRE2_SPTR) UPTIME_REGEXP, PCRE2_ZERO_TERMINATED, 0,
                                      &err_code, &err_offset, NULL);
    if (regex == NULL)
    {
        Log(LOG_LEVEL_DEBUG, "failed to compile regexp to parse uptime command");
        return(-1);
    }

    // Try "/usr/bin/uptime" first, then "/bin/uptime"
    uptimecmd = cf_popen(uptimepath, "r", false);
    uptimecmd = uptimecmd ? uptimecmd : cf_popen((uptimepath + 4), "r", false);
    if (!uptimecmd)
    {
        Log(LOG_LEVEL_ERR, "uptime failed: (cf_popen: %s)", GetErrorStr());
        pcre2_code_free(regex);
        return -1;
    }

    size_t uptime_output_size = CF_SMALLBUF;
    char *uptime_output = xmalloc(uptime_output_size);
    ssize_t n_read = CfReadLine(&uptime_output, &uptime_output_size, uptimecmd);

    cf_pclose(uptimecmd);
    if (n_read == -1 && !feof(uptimecmd))
    {
        Log(LOG_LEVEL_ERR, "Reading uptime output failed. (getline: '%s')", GetErrorStr());
        pcre2_code_free(regex);
        return -1;
    }

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(regex, NULL);
    if ((n_read > 0) &&
        (pcre2_match(regex, (PCRE2_SPTR) uptime_output, PCRE2_ZERO_TERMINATED,
                     0, 0, match_data, NULL) > 1))
    {
        size_t *ovector = pcre2_get_ovector_pointer(match_data);
        for (int i = 1; i <= UPTIME_BACKREFS ; i++)
        {
            if (ovector[i * 2 + 1] - ovector[i * 2] == 0) // strlen(backref)
            {
                continue;
            }
            backref = uptime_output + ovector[i * 2];
            // atoi() ignores non-digits, so no need to null-terminate backref

            time_t seconds;
            switch(i)
            {
                case 1: // Day
                    seconds = SECONDS_PER_DAY;
                    break;
                case 2: // Hour
                    seconds = SECONDS_PER_HOUR;
                    break;
                case 4: // Minute
                case 5:
                    seconds = SECONDS_PER_MINUTE;
                    break;
                default:
                    seconds = 0;
             }
             uptime += ((time_t) atoi(backref)) * seconds;
        }
    }
    else
    {
        Log(LOG_LEVEL_ERR, "uptime PCRE match failed: regexp: '%s', uptime: '%s'", UPTIME_REGEXP, uptime_output);
    }
    pcre2_match_data_free(match_data);
    pcre2_code_free(regex);
    Log(LOG_LEVEL_VERBOSE, "Reading boot time from uptime command successful.");
    return(uptime ? (now - uptime) : -1);
}

/*****************************************************************************/

/* TODO accept a const char * and move the ifdefs away from
 *      evalfunction.c:FnCallGetUserInfo() to here. */
JsonElement* GetUserInfo(const void *passwd)
{
#ifdef __MINGW32__
    return NULL;

#else /* !__MINGW32__ */

    // we lose the const to set pw if passwd is NULL
    struct passwd *pw = (struct passwd*) passwd;

    if (pw == NULL)
    {
        pw = getpwuid(getuid());
    }

    if (pw == NULL)
    {
        return NULL;
    }

    JsonElement *result = JsonObjectCreate(10);
    JsonObjectAppendString(result, "username", pw->pw_name);
    JsonObjectAppendString(result, "description", pw->pw_gecos);
    JsonObjectAppendString(result, "home_dir", pw->pw_dir);
    JsonObjectAppendString(result, "shell", pw->pw_shell);
    JsonObjectAppendInteger(result, "uid", pw->pw_uid);
    JsonObjectAppendInteger(result, "gid", pw->pw_gid);
    //JsonObjectAppendBool(result, "locked", IsAccountLocked(pw->pw_name, pw));
    // TODO: password: { format: "hash", data: { ...GetPasswordHash()... } }
    // TODO: group_primary: name of group
    // TODO: groups_secondary: [ names of groups ]
    // TODO: gids_secondary: [ gids of groups ]

    return result;
#endif
}

/*****************************************************************************/

void GetSysVars(EvalContext *ctx)
{
    /* Get info for current user. */
    JsonElement *info = GetUserInfo(NULL);
    if (info != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, "user_data",
                                      info, CF_DATA_TYPE_CONTAINER,
                                      "source=agent,user_info");
        JsonDestroy(info);
    }
}

/*****************************************************************************/

void GetDefVars(EvalContext *ctx)
{
    EvalContextVariablePutSpecial(
        ctx, SPECIAL_SCOPE_DEF, "jq",
        "jq --compact-output --monochrome-output --ascii-output --unbuffered --sort-keys",
        CF_DATA_TYPE_STRING, "invocation,source=agent,command_name=jq");
}

/*****************************************************************************/

static void SysOSNameHuman(EvalContext *ctx)
{
    // This function sets the $(sys.os_name_human) variable
    assert(ctx != NULL);
    const char *const lval = "os_name_human";

    /**
     * Order is important. The first if-statement is more specific and wins.
     * E.g. prefer Ubuntu over Debian.
     */
    if (EvalContextClassGet(ctx, NULL, "ubuntu") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "Ubuntu", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=ubuntu");
    }
    else if (EvalContextClassGet(ctx, NULL, "debian") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "Debian", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=debian");
    }
    else if (EvalContextClassGet(ctx, NULL, "centos") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "CentOS", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=centos");
    }
    else if (EvalContextClassGet(ctx, NULL, "fedora") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "Fedora", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=fedora");
    }
    else if (EvalContextClassGet(ctx, NULL, "redhat") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "RHEL", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=redhat");
    }
    else if (EvalContextClassGet(ctx, NULL, "aix") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "AIX", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=aix");
    }
    else if (EvalContextClassGet(ctx, NULL, "hpux") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "HP-UX", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=hpux");
    }
    else if (EvalContextClassGet(ctx, NULL, "opensuse") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "OpenSUSE", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=opensuse");
    }
    else if (EvalContextClassGet(ctx, NULL, "suse") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "SUSE", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=suse");
    }
    else if (EvalContextClassGet(ctx, NULL, "macos") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "macOS", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=macos");
    }
    else if (EvalContextClassGet(ctx, NULL, "windows") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "Windows", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=windows");
    }
    else if (EvalContextClassGet(ctx, NULL, "freebsd") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "FreeBSD", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=freebsd");
    }
    else if (EvalContextClassGet(ctx, NULL, "openbsd") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "OpenBSD", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=openbsd");
    }
    else if (EvalContextClassGet(ctx, NULL, "netbsd") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "NetBSD", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=netbsd");
    }
    else if (EvalContextClassGet(ctx, NULL, "solaris") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "Solaris", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=solaris");
    }
    else if (EvalContextClassGet(ctx, NULL, "amazon_linux") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "Amazon", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=amazon_linux");
    }
    else if (EvalContextClassGet(ctx, NULL, "manjaro") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "Manjaro", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=manjaro");
    }
    else if (EvalContextClassGet(ctx, NULL, "arch") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "Arch", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=arch");
    }
    else if (EvalContextClassGet(ctx, NULL, "postmarketos") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "postmarketOS", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=postmarketos");
    }
    else if (EvalContextClassGet(ctx, NULL, "alpine") != NULL)
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "Alpine", CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=alpine");
    }
    else
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS, lval,
                                      "Unknown", CF_DATA_TYPE_STRING,
                                      "source=agent");
        Log(LOG_LEVEL_WARNING,
            "Operating System not properly recognized, setting sys.os_name_human to \"Unknown\", please submit a bug report for us to fix this");
    }
}

/*****************************************************************************/

/**
 * Find next integer from string in place. Leading zero's are included.
 *
 * @param [in]  str string to extract next integer from
 * @param [out] num pointer to start of next integer or %NULL if no integer
 *                  number was found
 *
 * @return pointer to the remaining string in `str` or %NULL if no remainder
 *
 * @note `str` will be mutated
 */
static char *FindNextInteger(char *str, char **num)
{
    if (str == NULL)
    {
        *num = NULL;
        return NULL;
    }

    char *ptr = str;
    while (*ptr != '\0' && !isdigit(*ptr))
    {
        ++ptr;
    }
    if (*ptr == '\0')
    {
        /* no integer found */
        *num = NULL;
        return NULL;
    }
    *num = ptr++;

    while (*ptr != '\0' && isdigit(*ptr))
    {
        ++ptr;
    }
    if (*ptr == '\0')
    {
        /* no remainder */
        return NULL;
    }
    *ptr++ = '\0';

    return ptr;
}

static void SysOsVersionMajor(EvalContext *ctx)
{
    const char *const_flavor = EvalContextVariableGetSpecialString(
        ctx, SPECIAL_SCOPE_SYS, "flavor");
    char *flavor = SafeStringDuplicate(const_flavor);

    char *major;
    char *next = FindNextInteger(flavor, &major);

    if (flavor != NULL)
    {
        if (StringStartsWith(const_flavor, "solaris") ||
            StringStartsWith(const_flavor, "sunos"))
        {
            /* In this case the major version comes second.
             * E.g. SunOS 5.11 where 11 is the major release number.
             * Thus, we need to get the next number.
             */
            FindNextInteger(next, &major);
        }
    }

    if (NULL_OR_EMPTY(major))
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS,
                                      "os_version_major", "Unknown",
                                      CF_DATA_TYPE_STRING, "source=agent");
    }
    else
    {
        EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_SYS,
                                      "os_version_major", major,
                                      CF_DATA_TYPE_STRING,
                                      "source=agent,derived-from=flavor");
    }

    free(flavor);
}

/*****************************************************************************/

void DetectEnvironment(EvalContext *ctx)
{
    GetNameInfo3(ctx);
    GetInterfacesInfo(ctx);
    GetNetworkingInfo(ctx);
    Get3Environment(ctx);
    BuiltinClasses(ctx);
    OSClasses(ctx);
    GetSysVars(ctx);
    GetDefVars(ctx);
    SysOSNameHuman(ctx);
    SysOsVersionMajor(ctx);
}
