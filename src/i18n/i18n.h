#ifndef NAVMAP_I18N_H
#define NAVMAP_I18N_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 极简多语言：以中文为原文（msgid）。
 *   - zh 模式：i18n_tr(zh) 直接返回传入的 msgid（即中文本身）。
 *   - en 模式：查表返回英文；查不到则回退中文（绝不返回空串）。
 * 因此：默认中文下所有调用都是"原样返回"，不会引入任何字符串变化；
 * 既有的像素/UI 自检（断言中文串）在 zh 模式下行为与改前完全一致。
 * ------------------------------------------------------------------------- */

typedef enum {
    I18N_ZH = 0,   /* 中文（默认） */
    I18N_EN = 1    /* English */
} I18nLang;

void      i18n_set_lang(I18nLang l);
I18nLang  i18n_lang(void);
/* 以中文串为 key：返回当前语言的对应串（缺译回退中文）。 */
const char *i18n_tr(const char *zh);

#ifdef __cplusplus
}
#endif

#endif /* NAVMAP_I18N_H */
