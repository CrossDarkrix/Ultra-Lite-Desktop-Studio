#pragma once
#include "WidgetConfig.h"
#include <QString>

// JSHTML (html_js) package/directory support — a port of the Python
// build's litedesktopstudio/jshtml.py helpers that do NOT need
// WebEngine: the per-widget directory layout, config-field defaults
// and the traversal-safe ZIP package import. The actual rendering
// lives in the optional LdsJsHtml plugin (JsHtmlWidget.cpp loads it
// lazily); everything here is always compiled so package management
// works even in a build without WebEngine.
//
// Layout (identical to the Python build, so both can share widgets):
//   ~/LiteDesktopStudio_jshtml_widgets/<instance_id>/
//     assets/  data/ (data/config.json is the LDS API config store)
//     index.html / widget.json ... (package mode files)
namespace JsHtmlPackage {

// Fills jshtml_mode ("inline"), jshtml_entry ("index.html"),
// jshtml_permissions_json ("{}") and jshtml_instance_id (new UUID)
// defaults into cfg.extra. Returns true when anything changed.
bool ensureFields(WidgetConfig &cfg);

QString field(const WidgetConfig &cfg, const char *key);
void setField(WidgetConfig &cfg, const char *key, const QString &value);

// ~/LiteDesktopStudio_jshtml_widgets (created on demand).
QString baseDir();
// The widget's own directory; creates it plus assets/ and data/.
// Returns an empty string when the instance id escapes the base dir.
QString widgetDir(const WidgetConfig &cfg);

// Resolves `relative` inside `root` with the Python build's rules
// (backslashes normalized, leading "/" stripped, ":" rejected,
// ".." escapes rejected). Returns an empty string when unsafe.
QString safePath(const QString &root, const QString &relative);

// Extracts a package ZIP into destDir with the Python build's limits
// (<=1000 files, <=20 MB per file, <=50 MB total) and member-name
// safety rules. Returns false and sets `error` on any violation.
bool extractZip(const QString &zipPath, const QString &destDir, QString *error);

// Full import (port of import_jshtml_widget_package_to_config):
// clears the widget dir except data/, extracts, reads widget.json
// (entry / name / version / permissions), validates the entry file
// and switches cfg to package mode. On failure cfg is untouched
// except for the field defaults from ensureFields.
bool importPackage(WidgetConfig &cfg, const QString &zipPath, QString *error);

} // namespace JsHtmlPackage
