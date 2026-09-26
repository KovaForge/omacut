#pragma once

#include <QFileSystemWatcher>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <functional>
#include <memory>

#include "ffmpeg.h"
#include "sxcu.h"

class ThumbProvider;
class FilePicker;
class ThumbWorker;
class Uploader;
class QTemporaryDir;

// The bridge between QML and the ffmpeg/ffprobe layer. Holds the currently
// loaded video's info and drives thumbnail generation and export.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source NOTIFY infoChanged)
    Q_PROPERTY(double duration READ duration NOTIFY infoChanged)
    Q_PROPERTY(int thumbCount READ thumbCount NOTIFY thumbsChanged)
    Q_PROPERTY(int thumbReadyCount READ thumbReadyCount NOTIFY thumbsChanged)
    Q_PROPERTY(int thumbRevision READ thumbRevision NOTIFY thumbsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeAccentChanged)
    Q_PROPERTY(QString themeAccentForeground READ themeAccentForeground NOTIFY themeAccentChanged)
    Q_PROPERTY(QStringList uploadDestinations READ uploadDestinations NOTIFY uploadDestinationsChanged)
    Q_PROPERTY(int uploadDestination READ uploadDestination WRITE setUploadDestination NOTIFY uploadDestinationsChanged)
    Q_PROPERTY(QString uploadDestinationsDir READ uploadDestinationsDir CONSTANT)
    Q_PROPERTY(bool uploading READ uploading NOTIFY uploadingChanged)

public:
    explicit Backend(ThumbProvider *provider, QObject *parent = nullptr);
    explicit Backend(ThumbProvider *provider, FilePicker *filePicker,
                     QObject *parent = nullptr);
    ~Backend() override;

    QUrl source() const { return m_source; }
    double duration() const { return m_info.duration; }
    int thumbCount() const { return m_thumbCount; }
    int thumbReadyCount() const { return m_thumbReadyCount; }
    int thumbRevision() const { return m_thumbRevision; }
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeAccentForeground() const;
    QStringList uploadDestinations() const { return m_uploadDestinationNames; }
    int uploadDestination() const { return m_uploadDestination; }
    void setUploadDestination(int index);
    // Where user .sxcu files go, with the home directory shown as ~.
    QString uploadDestinationsDir() const;
    bool uploading() const { return m_uploading; }

    // The accent from an omarchy colors.toml, or the fallback when the file is
    // missing or holds no usable accent — which is what keeps omacut working on
    // distros without omarchy themes.
    static QString accentFromColorsFile(const QString &path, const QString &fallback);
    // "black" or "white", whichever stays legible on the given color.
    static QString foregroundFor(const QString &color);

    // Load a video (probes it, then kicks off thumbnail generation).
    Q_INVOKABLE bool load(const QUrl &url);

    // Open native desktop file dialogs.
    Q_INVOKABLE void openVideoDialog();
    Q_INVOKABLE void exportDialog(double start, double end);

    // Suggested "<name>_trimmed.mp4" target next to the source.
    Q_INVOKABLE QUrl suggestedExportUrl() const;

    // Write [start, end] (seconds) of the loaded video to dst. A non-zero
    // scaleHeight downscales the shorter side to that size.
    Q_INVOKABLE void exportClip(const QUrl &dst, double start, double end,
                                int scaleHeight = 0);

    // The downscale heights worth offering for a source: only ones strictly
    // below the source's shorter side, so exports never upscale.
    static QList<int> exportHeights(int width, int height);

    // Regenerate the filmstrip for [start, end] (seconds) — used by zoom.
    // The full-length strip is cached, so zooming back out restores instantly.
    Q_INVOKABLE void requestThumbs(double start, double end);

    // Re-read the built-in and user (.sxcu) upload destinations, keeping the
    // remembered choice.
    Q_INVOKABLE void reloadUploadDestinations();
    // The downscale heights an upload can pick from, like exportHeights.
    Q_INVOKABLE QList<int> uploadHeights() const;
    // Encode [start, end] like an export, send it to the chosen destination,
    // copy the link and record it in the upload history.
    Q_INVOKABLE void uploadClip(double start, double end, int scaleHeight = 0);
    Q_INVOKABLE void cancelUpload();

    // Where every successful upload is appended as a line of JSON.
    static QString uploadHistoryPath();
    // Puts text on the clipboard in a way that outlives omacut.
    static void copyToClipboard(const QString &text);
    // Replaces what happens to an uploaded link (tests keep the real
    // clipboard out of it).
    void setLinkCopier(std::function<void(const QString &)> copy) { m_copyLink = std::move(copy); }

signals:
    void infoChanged();
    void thumbsChanged();
    void busyChanged();
    void statusChanged();
    void themeAccentChanged();
    void exportDone(const QString &path);
    void exportFailed(const QString &message);
    void loadError(const QString &message);
    void uploadDestinationsChanged();
    void uploadingChanged();
    void uploadDone(const QString &url, const QString &deletionUrl);
    void uploadFailed(const QString &message);

private:
    void setBusy(bool busy);
    void setStatus(const QString &status);
    // Encodes [start, end] of the loaded video to outPath, reporting
    // "<verb> N%" as it goes. done gets an empty string on success; busy is
    // set here and left for done to clear.
    void encodeClip(const QString &outPath, double start, double end, int scaleHeight,
                    const QString &verb, std::function<void(const QString &)> done);
    void wireUploader();
    void finishUpload();
    void appendUploadHistory(const QString &url, const QString &thumbnailUrl,
                             const QString &deletionUrl) const;
    void startThumbs();
    void stopThumbs();
    void revealNextThumb();
    void wireFilePicker();
    void loadThemeAccent();
    void watchTheme();

    ThumbProvider *m_provider;
    FilePicker *m_filePicker;
    ThumbWorker *m_thumbWorker = nullptr;
    ffmpeg::VideoInfo m_info;
    QString m_path;
    QUrl m_source;
    double m_thumbStart = 0.0;
    double m_thumbLen = 0.0;
    QVector<QImage> m_fullThumbs;
    bool m_fullThumbsComplete = false;
    int m_thumbCount = 0;
    int m_thumbAvailableCount = 0;
    int m_thumbReadyCount = 0;
    int m_thumbRevision = 0;
    bool m_thumbWorkerDone = false;
    bool m_busy = false;
    QString m_status;
    QString m_themeAccent;
    QTimer m_thumbRevealTimer;
    QFileSystemWatcher m_themeWatcher;
    QPointer<QProcess> m_encoder;
    Uploader *m_uploader = nullptr;
    QList<sxcu::Destination> m_uploadDestinations;
    QStringList m_uploadDestinationNames;
    int m_uploadDestination = -1;
    QString m_uploadDestinationName;
    std::unique_ptr<QTemporaryDir> m_uploadDir;
    std::function<void(const QString &)> m_copyLink;
    bool m_uploading = false;
    bool m_uploadCancelled = false;
    double m_uploadStart = 0.0;
    double m_uploadEnd = 0.0;
};
