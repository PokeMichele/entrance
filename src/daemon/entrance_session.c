#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <grp.h>
#ifndef HAVE_PAM
#include <shadow.h>
#include <crypt.h>
#endif

#include "entrance.h"

#define HAVE_SHADOW 1

static char *_mcookie = NULL;
static const char *_dname = NULL;
static char **env;
static char *_login = NULL;
static unsigned char _logged = 0;
static pid_t _session_pid;
static Eina_List *_xsessions = NULL;
static int _entrance_session_sort(Entrance_Xsession *a, Entrance_Xsession *b);
static int _entrance_session_userid_set(struct passwd *pwd);

static void _entrance_session_run(struct passwd *pwd, const char *cmd, const char *cookie);

static void _entrance_session_desktops_scan_file(const char *path);
static void _entrance_session_desktops_scan(const char *dir);
static void _entrance_session_desktops_init(void);
static const char *_entrance_session_find_command(const char *path, const char *session);
static struct passwd *_entrance_session_session_open();

long
entrance_session_seed_get(void)
{
    struct timespec ts;
    long pid = getpid();
    long tm = time(NULL);
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
       ts.tv_sec = ts.tv_nsec = 0;
    return pid + tm + (ts.tv_sec ^ ts.tv_nsec);
}

static int
_entrance_session_cookie_add(const char *mcookie, const char *display,
                         const char *xauth_cmd, const char *auth_file)
{
    char buf[PATH_MAX];
    FILE *cmd;

    if (!xauth_cmd || !auth_file) return 1;
    snprintf(buf, sizeof(buf), "%s -f %s -q", xauth_cmd, auth_file);
    PT("write auth on display %s with file %s", display, auth_file);
    cmd = popen(buf, "w");
    if (!cmd)
      {
         PT("write auth fail !");
         return 1;
      }
    fprintf(cmd, "remove %s\n", display);
    fprintf(cmd, "add %s . %s\n", display, mcookie);
    fprintf(cmd, "exit\n");
    pclose(cmd);
    return 0;
}

static int
_entrance_session_userid_set(struct passwd *pwd)
{
   if (!pwd)
     {
        PT("no passwd !");
        return 1;
     }
   if (initgroups(pwd->pw_name, pwd->pw_gid) != 0)
     {
        PT("can't init group");
        return 1;
     }
   if (setgid(pwd->pw_gid) != 0)
     {
        PT("can't set gid");
        return 1;
     }
   if (setuid(pwd->pw_uid) != 0)
     {
        PT("can't set uid");
        return 1;
     }
   return 0;
}

static Eina_Bool
_entrance_session_begin(struct passwd *pwd, const char *cookie)
{
   PT("Session Init");
   if (pwd->pw_shell[0] == '\0')
     {
        setusershell();
        strncpy(pwd->pw_shell, getusershell(), sizeof(pwd->pw_shell)-1);
        endusershell();
     }
#ifdef HAVE_PAM
   char *term = NULL;
   char vtnr[128] = {0};
   
   term = getenv("TERM");
   eina_convert_xtoa(entrance_config->command.vtnr, vtnr);
   if (term) entrance_pam_env_set("TERM", term);
   entrance_pam_env_set("HOME", pwd->pw_dir);
   entrance_pam_env_set("SHELL", pwd->pw_shell);
   entrance_pam_env_set("USER", pwd->pw_name);
   entrance_pam_env_set("LOGNAME", pwd->pw_name);
   entrance_pam_env_set("PATH", entrance_config->session_path);
   entrance_pam_env_set("DISPLAY", _dname);
   entrance_pam_env_set("MAIL=/var/mail/%s", pwd->pw_name);
   entrance_pam_env_set("XAUTHORITY", cookie);
   entrance_pam_env_set("XDG_SEAT", "seat0");
   entrance_pam_env_set("XDG_SESSION_CLASS", "user");
   entrance_pam_env_set("XDG_VTNR", vtnr);
#endif
   return EINA_TRUE;
}

static void
_entrance_session_run(struct passwd *pwd, const char *cmd, const char *cookie)
{
   pid_t pid;
   pid = fork();
   if (pid == 0)
     {
        char buf[PATH_MAX];

        PT("Session Run");
#ifdef HAVE_PAM
        env = entrance_pam_env_list_get();
        entrance_pam_end();
#else
        int n = 0;
        char *term = getenv("TERM");
        env = (char **)malloc(12 * sizeof(char *));
        if(term)
          {
            char *t = NULL;
            t = strndup(term,128);
            if(t)
              {
                 snprintf(buf, sizeof(buf), "TERM=%s", t);
                 env[n++]=strdup(buf);
                 free(t);
              }
          }
        snprintf(buf, sizeof(buf), "HOME=%s", pwd->pw_dir);
        env[n++]=strdup(buf);
        snprintf(buf, sizeof(buf), "SHELL=%s", pwd->pw_shell);
        env[n++]=strdup(buf);
        snprintf(buf, sizeof(buf), "USER=%s", pwd->pw_name);
        env[n++]=strdup(buf);
        snprintf(buf, sizeof(buf), "LOGNAME=%s", pwd->pw_name);
        env[n++]=strdup(buf);
        snprintf(buf, sizeof(buf), "PATH=%s", entrance_config->session_path);
        env[n++]=strdup(buf);
        snprintf(buf, sizeof(buf), "DISPLAY=%s", _dname);
        env[n++]=strdup(buf);
        snprintf(buf, sizeof(buf), "MAIL=/var/mail/%s", pwd->pw_name);
        env[n++]=strdup(buf);
        snprintf(buf, sizeof(buf), "XAUTHORITY=%s", cookie);
        env[n++]=strdup(buf);
        snprintf(buf, sizeof(buf), "XDG_SESSION_CLASS=user");
        env[n++]=strdup(buf);
        snprintf(buf, sizeof(buf), "XDG_VTNR=%d", entrance_config->command.vtnr);
        env[n++]=strdup(buf);
        env[n++]=0;
#endif
        snprintf(buf, sizeof(buf),
                 "%s %s %s",
                 entrance_config->command.session_start,
                 _dname,
                 pwd->pw_name);
        if (-1 == system(buf))
          PT("Error on session start command");
        if(_entrance_session_userid_set(pwd)) return;
        _entrance_session_cookie_add(_mcookie, _dname,
                                 entrance_config->command.xauth_path, cookie);
        if (chdir(pwd->pw_dir))
          {
             PT("change directory for user fail");
             return;
          }
        snprintf(buf, sizeof(buf), "%s/.entrance_session.log", pwd->pw_dir);
        if (-1 == remove(buf))
          PT("Error could not remove session log file");

        snprintf(buf, sizeof(buf), "%s %s > %s/.entrance_session.log 2>&1",
                 entrance_config->command.session_login, cmd, pwd->pw_dir);

        PT("Executing: %s --login -c %s ", pwd->pw_shell, buf);
        execle(pwd->pw_shell, pwd->pw_shell, "--login", "-c", buf, NULL, env);
        PT("The Xsessions are not launched :(");
     }
   else if (pid > 0)
     {
        entrance_session_pid_set(pid);
     }
   else
     {
        PT("Failed to start session");
     }
}

void
entrance_session_close(const Eina_Bool opened)
{
#ifdef HAVE_PAM
   entrance_pam_close_session(opened);
   entrance_pam_end();
   entrance_pam_shutdown();
#endif
}

void
entrance_session_pid_set(pid_t pid)
{
   char buf[PATH_MAX];

   PT("%s: session pid %d", PACKAGE, pid);
   _session_pid = pid;
   snprintf(buf, sizeof(buf), "%d", pid);
   setenv("ENTRANCE_SPID", buf, 1);
}

pid_t
entrance_session_pid_get(void)
{
   return _session_pid;
}

void
entrance_session_init(const char *dname)
{
   _dname = dname;
}

static const char *dig = "0123456789abcdef";

void
entrance_session_cookie(void)
{
   uint16_t word;
   uint8_t hi;
   uint8_t lo;
   int i;
   char buf[PATH_MAX];

   _mcookie = calloc(33, sizeof(char));
   _mcookie[0] = 'a';

   long rand = 0;
   size_t read = 0;
   struct timespec time;
   FILE *fp;
   fp = fopen("/dev/urandom", "r");
   for (i=0; i<32; i+=4)
     {
       if (fp)
           read = fread(&rand,sizeof(rand),1,fp);
       if (read)
         {
           clock_gettime(CLOCK_REALTIME, &time);
           rand = time.tv_nsec;
         }
        word = rand & 0xffff;
        lo = word & 0xff;
        hi = word >> 8;
        _mcookie[i] = dig[lo & 0x0f];
        _mcookie[i+1] = dig[lo >> 4];
        _mcookie[i+2] = dig[hi & 0x0f];
        _mcookie[i+3] = dig[hi >> 4];
     }
   if(fp)
     fclose(fp);

   fp = fopen(entrance_config->command.xauth_file,"a+");
   if(!fp)
     {
       PT("unable to create xauth %s",entrance_config->command.xauth_file);
       return;
     }
   fclose(fp);

   snprintf(buf, sizeof(buf), "XAUTHORITY=%s",
            entrance_config->command.xauth_file);
   putenv(strdup(buf));
   _entrance_session_cookie_add(_mcookie, _dname,
                            entrance_config->command.xauth_path,
                            entrance_config->command.xauth_file);

   _entrance_session_desktops_init();
}

void
entrance_session_shutdown(void)
{
   Entrance_Xsession *xsession;

   EINA_LIST_FREE(_xsessions, xsession)
     {
        eina_stringshare_del(xsession->name);
        eina_stringshare_del(xsession->icon);
        if (xsession->command) eina_stringshare_del(xsession->command);
        free(xsession);
     }
   _session_pid = 0;
}

Eina_Bool
entrance_session_authenticate(const char *login, const char *passwd)
{
   Eina_Bool auth;
   _login = strdup(login);
#ifdef HAVE_PAM
   entrance_pam_init(_dname, login);
   auth = !!(!entrance_pam_passwd_set(passwd)
             && !entrance_pam_authenticate());
#else
   char *enc, *v;
   struct passwd *pwd;

   pwd = getpwnam(login);
   endpwent();
   if(!pwd)
     return EINA_FALSE;
#ifdef HAVE_SHADOW
   struct spwd *spd;
   spd = getspnam(pwd->pw_name);
   endspent();
   if(spd)
     v = spd->sp_pwdp;
   else
#endif
     v = pwd->pw_passwd;
   if(!v || *v == '\0')
     return EINA_TRUE;
   enc = crypt(passwd, v);
   auth = !strcmp(enc, v);
#endif
   eina_stringshare_del(passwd);
   memset((char *)passwd, 0, strlen(passwd));
   return auth;
}

static struct passwd *
_entrance_session_session_open(void)
{
#ifdef HAVE_PAM
   if (!entrance_pam_open_session())
      return getpwnam(entrance_pam_item_get(ENTRANCE_PAM_ITEM_USER));
   return NULL;
#else
   return getpwnam(entrance_session_login_get());
#endif
}

Eina_Bool
entrance_session_login(const char *session, Eina_Bool push)
{
   struct passwd *pwd;
   const char *cmd;
   char buf[PATH_MAX];

   pwd = _entrance_session_session_open();
   endpwent();
   if (!pwd) return ECORE_CALLBACK_CANCEL;
   _logged = EINA_TRUE;
   snprintf(buf, sizeof(buf), "%s/.Xauthority", pwd->pw_dir);
   if (!_entrance_session_begin(pwd, buf))
     {
        PT("Entrance: couldn't open session");
        exit(1);
     }
   if (push) entrance_history_push(pwd->pw_name, session);
   cmd = _entrance_session_find_command(pwd->pw_dir, session);
   if (!cmd)
     {
        PT("Error !!! No command to launch, can't open a session :'(");
        return EINA_FALSE;
     }
   if(!_login && entrance_auto_login_enabled())
     _login = strdup(entrance_config->userlogin);
   PT("launching session %s for user %s", cmd, _login);
   _entrance_session_run(pwd, cmd, buf);
   free(_login);
   return EINA_TRUE;
}

static const char *
_entrance_session_find_command(const char *path, const char *session)
{
   Entrance_Xsession *xsession = NULL;
   char buf[PATH_MAX];
   if (session)
     {
        Eina_List *l;

        EINA_LIST_FOREACH(_xsessions, l, xsession)
          {
             if (!strcmp(xsession->name, session) && xsession->command)
               return xsession->command;
          }
     }
   snprintf(buf, sizeof(buf), "%s/%s",
            path, ".xinitrc");
   if (ecore_file_can_exec(buf))
     {
        if (xsession)
          snprintf(buf, sizeof(buf), "%s/%s %s",
                   path, ".xinitrc", xsession->command);
        return eina_stringshare_add(buf);
     }
   snprintf(buf, sizeof(buf), "%s/%s",
            path, ".Xsession");
   if (ecore_file_can_exec(buf))
     {
        if (xsession)
          snprintf(buf, sizeof(buf), "%s/%s %s",
                   path, ".Xsession", xsession->command);
        return eina_stringshare_add(buf);
     }
   if (ecore_file_exists("/etc/X11/xinit/xinitrc"))
     {
        if (xsession)
          {
             snprintf(buf, sizeof(buf), "sh /etc/X11/xinit/xinitrc %s",
                      xsession->command);
             return eina_stringshare_add(buf);
          }
        return eina_stringshare_add("sh /etc/X11/xinit/xinitrc");
     }
   return NULL;
}

char *
entrance_session_login_get(void)
{
   return _login;
}

int
entrance_session_logged_get(void)
{
   return !!_logged;
}

Eina_List *
entrance_session_list_get(void)
{
   return _xsessions;
}

static void
_entrance_session_desktops_init(void)
{
   efreet_desktop_type_alias(EFREET_DESKTOP_TYPE_APPLICATION, "XSession");
   _entrance_session_desktops_scan("/etc/X11/Sessions");
   _entrance_session_desktops_scan("/usr/share/xsessions");
   _entrance_session_desktops_scan("/usr/share/wayland-sessions");
}

static void
_entrance_session_desktops_scan(const char *dir)
{
   if (ecore_file_is_dir(dir))
     {
        Eina_List *files;
        char *filename;
        char path[PATH_MAX];

        files = ecore_file_ls(dir);
        EINA_LIST_FREE(files, filename)
          {
             snprintf(path, sizeof(path), "%s/%s", dir, filename);
             _entrance_session_desktops_scan_file(path);
             free(filename);
          }
     }
}

static void
_entrance_session_desktops_scan_file(const char *path)
{
   Efreet_Desktop *desktop;
   Eina_List *commands;
   Eina_List *l;
   Entrance_Xsession *xsession;
   char *command = NULL;

   desktop = efreet_desktop_get(path);
   if (!desktop) return;
   EINA_LIST_FOREACH(_xsessions, l, xsession)
     {
        if (!strcmp(xsession->name, desktop->name))
          {
             efreet_desktop_free(desktop);
             return;
          }
     }

   commands = efreet_desktop_command_local_get(desktop, NULL);
   if (commands)
     command = eina_list_data_get(commands);
   if (command && desktop->name)
     {
        PT("Adding %s as wm", desktop->name);
        xsession= calloc(1, sizeof(Entrance_Xsession));
        xsession->command = eina_stringshare_add(command);
        xsession->name = eina_stringshare_add(desktop->name);
        if (desktop->icon && strcmp(desktop->icon,""))
          xsession->icon = eina_stringshare_add(desktop->icon);
        _xsessions = eina_list_sorted_insert(_xsessions,
            (Eina_Compare_Cb)_entrance_session_sort,
            xsession);
     }
   EINA_LIST_FREE(commands, command)
     free(command);
   efreet_desktop_free(desktop);
}

static int
_entrance_session_sort(Entrance_Xsession *a, Entrance_Xsession *b)
{
    return strcasecmp(a->name, b->name);
}
