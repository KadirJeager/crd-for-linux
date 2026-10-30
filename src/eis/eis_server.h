#ifndef CRD_EIS_SERVER_H
#define CRD_EIS_SERVER_H

struct crd_stream_info;

int eis_server_init(void);
int eis_client_fd(void);
/* Portal, aygıt kurulmadan önce yakalamanın ölçüsünü bağlar.
 * NULL ise bölge Wayland seçiminden okunur. */
void eis_server_set_capture_info(int (*stream_info)(struct crd_stream_info *out));
/* Her çağrı yeni bir istemci soketi döner. Çağıran kapatır. */
int eis_open_client_fd(void);

#endif
