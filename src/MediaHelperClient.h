#pragma once
#include <QImage>
#include <QObject>
#include <QString>

// Snapshot of "now playing" as reported by the Python media helper.
struct MediaInfo {
    bool available = false;   // helper is running and answered
    bool session = false;     // a media session exists
    QString title, artist, album, app, status; // status: Playing/Paused/...
    qint64 positionMs = 0, durationMs = 0;
    QImage artwork;           // album cover (may be null)
};

// Bridge to LiteDesktopStudio-MediaHelper.exe (see helper/README_media_helper.md).
//
// The C++ build cannot call WinRT/GSMTC from MinGW, so media metadata is
// fetched by a small Python helper (Nuitka onedir build) that lives in
// <app dir>/MediaHelper/. It is started on demand: the first acquire()
// launches it, the last release() stops it (closing its stdin makes it
// exit by itself). While nobody needs metadata it costs nothing.
//
// The helper writes one JSON object per line on stdout; this class parses
// them and republishes changes through mediaChanged().
class MediaHelperClient : public QObject {
    Q_OBJECT
public:
    static MediaHelperClient *instance();
    void acquire();
    void release();
    MediaInfo info() const { return info_; }
    bool helperPresent() const;
    // Transport control goes through a SEPARATE one-shot helper
    // (LiteDesktopStudio-MediaControlHelper.exe, see
    // helper/media_control_helper.py): synthesized media keys did not
    // move the player on the target machine, GSMTC session calls do.
    bool controlHelperPresent() const;
    void sendTransportCommand(const QString &cmd); // play|pause|play_pause|next|prev|stop
signals:
    void mediaChanged();
private:
    explicit MediaHelperClient(QObject *parent = nullptr);
    ~MediaHelperClient() override;
    void startHelper();
    void stopHelper();
    void handleLine(const QByteArray &line);
    class QProcess *proc_ = nullptr;
    QByteArray buffer_;
    MediaInfo info_;
    int refs_ = 0;
    bool announced_ = false;
};
