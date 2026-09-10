#ifndef EDGEGUARD_PROVISIONING_PROTOCOL_H
#define EDGEGUARD_PROVISIONING_PROTOCOL_H

#include "model.h"

G_BEGIN_DECLS

#define EGP_PROTOCOL_VERSION 1u
#define EGP_FRAME_HEADER_BYTES 32u
#define EGP_PROTOCOL_MAX_JSON 1024u
#define EGP_PROTOCOL_MAX_INCOMPLETE 8u
#define EGP_PROTOCOL_IDLE_TIMEOUT_US (30 * G_USEC_PER_SEC)
#define EGP_PROTOCOL_LIFETIME_US (60 * G_USEC_PER_SEC)
#define EGP_PROTOCOL_REPLAY_TTL_US (10 * 60 * G_USEC_PER_SEC)

#define EGP_SERVICE_UUID "8a1d4e59-84e0-56d3-8d97-2080e5e77791"
#define EGP_DEVICE_INFO_UUID "6a174d82-0b81-5fa2-bca8-1cef78011280"
#define EGP_RUNTIME_STATUS_UUID "ed5101e3-0964-5afa-b52c-c653c2c1e3ca"
#define EGP_PROVISIONING_REQUEST_UUID "9f6e55ea-003a-5254-95dd-5806ad2fb97d"
#define EGP_OPERATION_RESULT_UUID "6b26b3c6-d089-5b53-9fea-5e5524ce169c"
#define EGP_CONTROL_REQUEST_UUID "413bd486-a58e-5947-b570-9cffba5eec5c"

typedef enum {
    EGP_WRITABLE_PROVISIONING,
    EGP_WRITABLE_CONTROL
} EgpWritableCharacteristic;

typedef enum {
    EGP_OPCODE_SET_WIFI = 0x01,
    EGP_OPCODE_SET_ENDPOINT = 0x02,
    EGP_OPCODE_FORGET_WIFI = 0x03,
    EGP_OPCODE_CHECK_UPDATE_NOW = 0x10
} EgpOpcode;

typedef struct {
    char peer_path[EGP_DBUS_PATH_CAP];
    EgpWritableCharacteristic characteristic;
    guint8 transaction_id[16];
    /* Volatile authorization tokens captured from the encrypted GATT write.
     * They are never supplied by the peer or serialized on either wire. */
    guint64 window_epoch;
    guint64 connection_epoch;
    guint8 opcode;
    guint16 json_length;
    char json[EGP_PROTOCOL_MAX_JSON + 1u];
} EgpProtocolRequest;

typedef enum {
    EGP_PROTOCOL_INCOMPLETE,
    EGP_PROTOCOL_COMPLETE,
    EGP_PROTOCOL_REPLAY
} EgpProtocolDisposition;

typedef struct {
    EgpProtocolDisposition disposition;
    EgpProtocolRequest request;
    char cached_result[EGP_PROTOCOL_MAX_JSON + 1u];
} EgpProtocolOutput;

typedef struct _EgpProtocol EgpProtocol;

EgpProtocol *egp_protocol_new(void);
void egp_protocol_free(EgpProtocol *protocol);

/* now_us is CLOCK_MONOTONIC expressed in microseconds. accepted fragments are
 * copied into secret-bearing buffers owned and deterministically cleared by
 * the protocol engine. */
gboolean egp_protocol_accept(EgpProtocol *protocol, const char *peer_path,
                             EgpWritableCharacteristic characteristic,
                             const guint8 *value, gsize value_length,
                             gint64 now_us, EgpProtocolOutput *output,
                             EgpError *error);
void egp_protocol_expire(EgpProtocol *protocol, gint64 now_us);
/* Disconnect clears plaintext/incomplete state only. Completed digest/result
 * cache entries survive for authorized reconnect reads until their TTL. */
void egp_protocol_drop_peer(EgpProtocol *protocol, const char *peer_path);
void egp_protocol_drop_all(EgpProtocol *protocol);

/* Cache only the digest and redacted result for a completed request. The
 * plaintext request is never retained after dispatch. */
gboolean egp_protocol_cache_result(EgpProtocol *protocol,
                                   const EgpProtocolRequest *request,
                                   const char *result_json, gint64 now_us,
                                   EgpError *error);
gboolean egp_protocol_cache_result_id(EgpProtocol *protocol,
                                      const char *peer_path,
                                      EgpWritableCharacteristic characteristic,
                                      const guint8 transaction_id[16],
                                      const char *result_json, gint64 now_us,
                                      EgpError *error);
void egp_transaction_id_format(const guint8 id[16], char output[37]);
const char *egp_opcode_name(guint8 opcode);

G_END_DECLS

#endif
