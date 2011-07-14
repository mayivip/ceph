#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <curl/curl.h>

#include "fcgiapp.h"

#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "common/config.h"
#include "common/errno.h"
#include "rgw_common.h"
#include "rgw_access.h"
#include "rgw_acl.h"
#include "rgw_user.h"
#include "rgw_op.h"
#include "rgw_rest.h"
#include "rgw_os.h"
#include "rgw_log.h"

#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>

#include "include/types.h"
#include "common/BackTrace.h"

using namespace std;

static sighandler_t sighandler_usr1;
static sighandler_t sighandler_alrm;

static void godown_handler(int signum)
{
  FCGX_ShutdownPending();
  signal(signum, sighandler_usr1);
  alarm(5);
}

static void godown_alarm(int signum)
{
  _exit(0);
}

/*
 * start up the RADOS connection and then handle HTTP messages as they come in
 */
int main(int argc, const char **argv)
{
  struct req_state s;
  struct fcgx_state fcgx;

  curl_global_init(CURL_GLOBAL_ALL);

  // dout() messages will be sent to stderr, but FCGX wants messages on stdout
  // Redirect stderr to stdout.
  TEMP_FAILURE_RETRY(close(STDERR_FILENO));
  if (TEMP_FAILURE_RETRY(dup2(STDOUT_FILENO, STDERR_FILENO) < 0)) {
    int err = errno;
    cout << "failed to redirect stderr to stdout: " << cpp_strerror(err)
	 << std::endl;
    return ENOSYS;
  }

  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);
  global_init(args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  RGWStoreManager store_manager;

  if (!store_manager.init("rados", g_ceph_context)) {
    derr << "Couldn't init storage provider (RADOS)" << dendl;
    return EIO;
  }

  sighandler_usr1 = signal(SIGUSR1, godown_handler);
  sighandler_alrm = signal(SIGALRM, godown_alarm);

  while (FCGX_Accept(&fcgx.in, &fcgx.out, &fcgx.err, &fcgx.envp) >= 0) 
  {
    rgw_env.reinit(fcgx.envp);

    RGWOp *op;
    int init_error = 0;
    RGWHandler *handler = RGWHandler_REST::init_handler(&s, &fcgx, &init_error);
    int ret;
    
    if (init_error != 0) {
      abort_early(&s, init_error);
      goto done;
    }

    if (!handler->authorize(&s)) {
      RGW_LOG(10) << "failed to authorize request" << dendl;
      abort_early(&s, -EPERM);
      goto done;
    }
    if (s.user.suspended) {
      RGW_LOG(10) << "user is suspended, uid=" << s.user.user_id << dendl;
      abort_early(&s, -ERR_USER_SUSPENDED);
      goto done;
    }
    ret = handler->read_permissions();
    if (ret < 0) {
      abort_early(&s, ret);
      goto done;
    }

    op = handler->get_op();
    if (op) {
      ret = op->verify_permission();
      if (ret < 0) {
        abort_early(&s, ret);
        goto done;
      }

      if (s.expect_cont)
        dump_continue(&s);

      op->execute();
    } else {
      abort_early(&s, -ERR_METHOD_NOT_ALLOWED);
    }
done:
    rgw_log_op(&s);
  }
  return 0;
}

