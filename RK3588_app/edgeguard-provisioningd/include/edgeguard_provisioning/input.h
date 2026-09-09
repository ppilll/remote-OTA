#ifndef EDGEGUARD_PROVISIONING_INPUT_H
#define EDGEGUARD_PROVISIONING_INPUT_H

#include "model.h"

G_BEGIN_DECLS

typedef struct _EgpInput EgpInput;
typedef void (*EgpInputActivated)(gpointer user_data);
typedef void (*EgpInputLost)(gpointer user_data);

/* key_code==0 or hold_ms==0 intentionally disables physical-presence input.
 * event devices are discovered by the exact EVIOCGNAME value "adc-keys";
 * ambiguous discovery fails closed and no /dev/input/eventN is hard-coded. */
EgpInput *egp_input_new(guint key_code, guint hold_ms,
                        EgpInputActivated activated, EgpInputLost lost,
                        gpointer user_data, EgpError *error);
void egp_input_free(EgpInput *input);

G_END_DECLS

#endif
