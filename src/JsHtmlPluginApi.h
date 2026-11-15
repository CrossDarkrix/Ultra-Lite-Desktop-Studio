#pragma once
// C ABI between the main executable (JsHtmlWidget, which must never
// link WebEngine) and the optional LdsJsHtml plugin (the only piece
// that links Qt WebEngine). The plugin is loaded with QLibrary the
// first time an html_js widget appears, so a setup without any
// JSHTML widget pays exactly zero for Chromium — the project's
// "unused features cost nothing" principle.
//
// All strings are UTF-8 JSON. The view returned by lds_js_create is
// a QWidget parented to the host widget window: the host drives its
// geometry/visibility and may simply let Qt delete it with the
// parent; the shared WebEngine profile outlives all views inside
// the plugin.

class QWidget;

// Called (on the GUI thread) when the page receives a right-click:
// the host opens the widget's normal context menu at that position.
using LdsJsContextMenuFn = void (*)(void *ctx, int globalX, int globalY);

using LdsJsCreateFn = void *(*)(QWidget *parent, const char *cfgJsonUtf8,
                                LdsJsContextMenuFn menuFn, void *menuCtx);
using LdsJsReloadFn = void (*)(void *view, const char *cfgJsonUtf8);
using LdsJsPushStateFn = void (*)(void *view, const char *stateJsonUtf8);
using LdsJsSetMouseTransparentFn = void (*)(void *view, int on);

inline const char *ldsJsCreateSymbol() { return "lds_js_create"; }
inline const char *ldsJsReloadSymbol() { return "lds_js_reload"; }
inline const char *ldsJsPushStateSymbol() { return "lds_js_push_state"; }
inline const char *ldsJsSetMouseTransparentSymbol() { return "lds_js_set_mouse_transparent"; }
