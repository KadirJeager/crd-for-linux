#ifndef CRD_EIS_KUYRUK_H
#define CRD_EIS_KUYRUK_H

struct eis_event;

/* eis_server olay döngüsü eis_get_event ile kuyruğu tüketir ve giriş
 * olayını dışarı vermez. Bu dosya libcrd_eis.a içine girmez.
 *
 * Bağ: -Wl,--wrap=eis_get_event
 * __wrap_eis_get_event, çeviricinin anladığı olayı (hareket, tuş, düğme,
 * tekerlek, çerçeve) işleyiciye verir; protokol olayını sunucuya bırakır.
 * İşleyici EIS dağıtım ipliğindedir ve kilit tutulurken çağrılır.
 * İşleyici eis_get_event, eis_kuyruk_seal veya bu kilidi yeniden almamalı.
 * Olay referansı işleyicinin değildir; saklanmaz.
 */
typedef void (*eis_kuyruk_handler)(struct eis_event *event, void *user);

void eis_kuyruk_set_handler(eis_kuyruk_handler handler, void *user);

/* Bundan sonra gelen giriş olayı işleyiciye gitmez. */
void eis_kuyruk_seal(void);

#endif
