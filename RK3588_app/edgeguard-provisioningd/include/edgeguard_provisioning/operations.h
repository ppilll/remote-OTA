#ifndef EDGEGUARD_PROVISIONING_OPERATIONS_H
#define EDGEGUARD_PROVISIONING_OPERATIONS_H

#include "connman.h"
#include "protocol.h"
#include "security.h"
#include "status.h"

G_BEGIN_DECLS

typedef struct _EgpOperations EgpOperations;

typedef gboolean (*EgpAuthorizeCommit)(gpointer user_data, const char *peer_path,
                                       guint64 window_epoch,
                                       guint64 connection_epoch,
                                       EgpError *error);
typedef void (*EgpResultPublished)(gpointer user_data, const char *peer_path,
                                   const char *result_json);

EgpOperations *egp_operations_new(EgpStore *store, EgpConnman *connman,
                                  EgpStatus *status,
                                  EgpAuthorizeCommit authorize_commit,
                                  EgpResultPublished result_published,
                                  gpointer user_data);
void egp_operations_free(EgpOperations *operations);

/* Ownership of request contents is copied. The caller may clear its request
 * immediately. At most one operation owns the mutation worker at a time. */
gboolean egp_operations_submit(EgpOperations *operations,
                               const EgpProtocolRequest *request,
                               EgpError *error);
gboolean egp_operations_busy(EgpOperations *operations);

typedef void (*EgpReconcileCompleted)(gpointer user_data, EgpErrorCode code);
gboolean egp_operations_reconcile(EgpOperations *operations,
                                  EgpReconcileCompleted completed,
                                  gpointer completed_data,
                                  EgpError *error);
void egp_operations_connman_lost(EgpOperations *operations);

G_END_DECLS

#endif
