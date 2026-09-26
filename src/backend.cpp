#include "backend.h"

#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>
#include <memory>

#include "filepicker.h"
#include "portalfilepicker.h"
#include "thumbprovider.h"
#include "thumbworker.h"
#include "uploader.h"

namespace {
constexpr int kThumbCount = 12;
constexpr int kThumbRevealMs = 70;
const QString kDefaultAccent = QStringLiteral("#FFD60A");
const QString kUploadDestinationKey = QStringLiteral("upload/destination");
// Settings live in ~/.config/omacut/omacut.conf, beside the user's uploaders.
const QString kSettingsName = QStringLiteral("omacut");

QString omarchyCurrentDir() {
    return QDir::homePath() + QStringLiteral("/.local/state/omarchy/current");
}

QString omarchyColorsPath() {
    return omarchyCurrentDir() + QStringLiteral("/theme/colors.toml");
}

QString mp4PathFor(const QString &path) {
    const QFileInfo file(path);
    if (file.suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) == 0)
        return path;

    const QString baseName = file.completeBaseName().isEmpty()
        ? file.fileName()
        : file.completeBaseName();
    return file.dir().filePath(baseName + QStringLiteral(".mp4"));
}

bool replaceWithTemp(const QString &tmpPath, const QString &outPath) {
    const QByteArray tmpName = QFile::encodeName(tmpPath);
    const QByteArray outName = QFile::encodeName(outPath);
    return std::rename(tmpName.constData(), outName.constData()) == 0;
}
}

Backend::Backend(ThumbProvider *provider, QObject *parent)
    : Backend(provider, new PortalFilePicker(), parent) {}

Backend::Backend(ThumbProvider *provider, FilePicker *filePicker, QObject *parent)
    : QObject(parent), m_provider(provider), m_filePicker(filePicker),
      m_themeAccent(kDefaultAccent) {
    if (!m_filePicker->parent())
        m_filePicker->setParent(this);
    wireFilePicker();
    wireUploader();
    m_copyLink = &Backend::copyToClipboard;
    reloadUploadDestinations();
    m_thumbRevealTimer.setInterval(kThumbRevealMs);
    connect(&m_thumbRevealTimer, &QTimer::timeout, this, &Backend::revealNextThumb);

    // Follow omarchy theme switches live. The theme lives behind a symlink that
    // gets swapped, so the reload also re-arms the watch paths every time.
    const auto themeChanged = [this] {
        watchTheme();
        loadThemeAccent();
    };
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, this, themeChanged);
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, this, themeChanged);
    watchTheme();
    loadThemeAccent();
}

Backend::~Backend() {
    stopThumbs();
}

void Backend::wireFilePicker() {
    connect(m_filePicker, &FilePicker::openSelected, this, &Backend::load);
    connect(m_filePicker, &FilePicker::exportSelected, this, &Backend::exportClip);
    connect(m_filePicker, &FilePicker::failed, this, &Backend::loadError);
}

void Backend::wireUploader() {
    m_uploader = new Uploader(this);
    connect(m_uploader, &Uploader::progress, this, [this](qint64 sent, qint64 total) {
        if (total > 0)
            setStatus(QStringLiteral("Uploading %1%").arg(qBound(0, int(sent * 100 / total), 100)));
    });
    connect(m_uploader, &Uploader::finished, this,
            [this](const QString &url, const QString &thumbnailUrl, const QString &deletionUrl) {
                if (m_copyLink)
                    m_copyLink(url);
                appendUploadHistory(url, thumbnailUrl, deletionUrl);
                finishUpload();
                emit uploadDone(url, deletionUrl);
            });
    connect(m_uploader, &Uploader::failed, this, [this](const QString &message) {
        finishUpload();
        emit uploadFailed(message);
    });
}

void Backend::setBusy(bool busy) {
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

QString Backend::accentFromColorsFile(const QString &path, const QString &fallback) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return fallback;

    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        const int equals = line.indexOf(QLatin1Char('='));
        if (equals < 0 || line.left(equals).trimmed() != QStringLiteral("accent"))
            continue;

        QString value = line.mid(equals + 1).trimmed();
        if (value.size() >= 2
                && ((value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
                    || (value.front() == QLatin1Char('\'') && value.back() == QLatin1Char('\''))))
            value = value.mid(1, value.size() - 2);

        return QColor::fromString(value).isValid() ? value : fallback;
    }
    return fallback;
}

QString Backend::foregroundFor(const QString &color) {
    const QColor parsed = QColor::fromString(color);
    if (!parsed.isValid())
        return QStringLiteral("black");
    const double luminance = 0.299 * parsed.redF()
        + 0.587 * parsed.greenF() + 0.114 * parsed.blueF();
    return luminance < 0.5 ? QStringLiteral("white") : QStringLiteral("black");
}

QString Backend::themeAccentForeground() const {
    return foregroundFor(m_themeAccent);
}

void Backend::loadThemeAccent() {
    const QString accent = accentFromColorsFile(omarchyColorsPath(), kDefaultAccent);
    if (accent == m_themeAccent)
        return;
    m_themeAccent = accent;
    emit themeAccentChanged();
}

void Backend::watchTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const QString currentDir = omarchyCurrentDir();
    const QString themeDir = currentDir + QStringLiteral("/theme");
    if (QDir(currentDir).exists())
        m_themeWatcher.addPath(currentDir);
    if (QDir(themeDir).exists())
        m_themeWatcher.addPath(themeDir);
    if (QFileInfo::exists(omarchyColorsPath()))
        m_themeWatcher.addPath(omarchyColorsPath());
}

bool Backend::load(const QUrl &url) {
    const QString path = url.toLocalFile();
    const ffmpeg::VideoInfo info = ffmpeg::probe(path);
    if (!info.ok) {
        emit loadError(info.error);
        return false;
    }

    m_info = info;
    m_path = path;
    m_source = url;

    // New video: drop the old filmstrip and bump the revision so QML reloads.
    stopThumbs();
    m_thumbStart = 0.0;
    m_thumbLen = m_info.duration;
    m_fullThumbs = QVector<QImage>(kThumbCount);
    m_fullThumbsComplete = false;
    m_thumbCount = kThumbCount;
    m_thumbAvailableCount = 0;
    m_thumbReadyCount = 0;
    m_thumbWorkerDone = false;
    ++m_thumbRevision;
    m_provider->setImages(QVector<QImage>(kThumbCount));
    emit thumbsChanged();

    emit infoChanged();

    setStatus(QStringLiteral("Loading..."));
    startThumbs();
    return true;
}

void Backend::openVideoDialog() {
    m_filePicker->openVideo();
}

void Backend::exportDialog(double start, double end) {
    if (m_path.isEmpty() || !m_info.ok)
        return;

    m_filePicker->exportVideo(suggestedExportUrl(), start, end,
                              exportHeights(m_info.width, m_info.height));
}

QList<int> Backend::exportHeights(int width, int height) {
    const int shortSide = qMin(width, height);
    QList<int> heights;
    for (const int candidate : {1080, 720}) {
        if (shortSide > candidate)
            heights << candidate;
    }
    return heights;
}

void Backend::startThumbs() {
    auto *worker = new ThumbWorker(m_path, m_thumbStart, m_thumbLen, kThumbCount);
    m_thumbWorker = worker;
    // Pair the pointer check with the revision: a recycled worker address could
    // otherwise let a stale queued callback write into the new filmstrip.
    const int revision = m_thumbRevision;

    connect(worker, &ThumbWorker::thumbReady, this, [this, worker, revision](int index, const QImage &image) {
        if (worker != m_thumbWorker || revision != m_thumbRevision)
            return;
        m_provider->setImage(index, image);
        // Thumbs arrive in order, so the strip is fully cached at the last one.
        if (m_thumbStart <= 0.0 && m_thumbLen >= m_info.duration) {
            m_fullThumbs[index] = image;
            if (index == kThumbCount - 1)
                m_fullThumbsComplete = true;
        }
        m_thumbAvailableCount = qMax(m_thumbAvailableCount, index + 1);
        if (m_thumbReadyCount == 0)
            revealNextThumb();
        if (!m_thumbRevealTimer.isActive())
            m_thumbRevealTimer.start();
    });
    connect(worker, &ThumbWorker::finished, this, [this, worker, revision] {
        if (worker == m_thumbWorker && revision == m_thumbRevision) {
            m_thumbWorker = nullptr;
            m_thumbWorkerDone = true;
            if (m_thumbReadyCount >= m_thumbCount)
                setStatus(QString());
            else if (!m_thumbRevealTimer.isActive())
                m_thumbRevealTimer.start();
        }
        worker->deleteLater();
    });
    worker->start();
}

void Backend::revealNextThumb() {
    if (m_thumbReadyCount < m_thumbAvailableCount) {
        ++m_thumbReadyCount;
        emit thumbsChanged();
    }

    if (m_thumbReadyCount < m_thumbAvailableCount)
        return;

    m_thumbRevealTimer.stop();
    if (m_thumbWorkerDone && m_thumbReadyCount >= m_thumbCount)
        setStatus(QString());
}

void Backend::stopThumbs() {
    m_thumbRevealTimer.stop();
    if (!m_thumbWorker)
        return;

    ThumbWorker *worker = m_thumbWorker;
    m_thumbWorker = nullptr;
    worker->disconnect(this);
    worker->requestStop();
    worker->wait();
    delete worker;
}

void Backend::requestThumbs(double start, double end) {
    if (m_path.isEmpty() || !m_info.ok)
        return;
    start = qBound(0.0, start, m_info.duration);
    end = qBound(start, end, m_info.duration);
    if (end - start <= 0.0 || (start == m_thumbStart && end - start == m_thumbLen))
        return;

    stopThumbs();
    m_thumbStart = start;
    m_thumbLen = end - start;
    ++m_thumbRevision;

    // Zooming back out: restore the cached full-length strip instantly.
    if (start <= 0.0 && end >= m_info.duration && m_fullThumbsComplete) {
        m_provider->setImages(m_fullThumbs);
        m_thumbAvailableCount = kThumbCount;
        m_thumbReadyCount = kThumbCount;
        m_thumbWorkerDone = true;
        emit thumbsChanged();
        return;
    }

    m_thumbAvailableCount = 0;
    m_thumbReadyCount = 0;
    m_thumbWorkerDone = false;
    m_provider->setImages(QVector<QImage>(kThumbCount));
    emit thumbsChanged();
    startThumbs();
}

QUrl Backend::suggestedExportUrl() const {
    if (m_path.isEmpty())
        return {};
    const QFileInfo src(m_path);
    const QString target = src.dir().filePath(src.completeBaseName() + "_trimmed.mp4");
    return QUrl::fromLocalFile(target);
}

void Backend::exportClip(const QUrl &dst, double start, double end, int scaleHeight) {
    if (m_path.isEmpty() || !m_info.ok || m_busy)
        return;

    if (end - start <= 0.0) {
        emit exportFailed("The selected clip has no length.");
        return;
    }

    // Forcing the .mp4 suffix can redirect the write to a file the save
    // dialog never asked the user about overwriting — refuse rather than
    // silently replace it.
    const QString selectedPath = dst.toLocalFile();
    const QString outPath = mp4PathFor(selectedPath);
    if (outPath != selectedPath && QFileInfo::exists(outPath)) {
        emit exportFailed(QStringLiteral("%1 already exists.")
                              .arg(QFileInfo(outPath).fileName()));
        return;
    }

    encodeClip(outPath, start, end, scaleHeight, QStringLiteral("Exporting"),
               [this, outPath](const QString &error) {
                   setBusy(false);
                   setStatus(QString());
                   if (error.isEmpty())
                       emit exportDone(outPath);
                   else
                       emit exportFailed(error);
               });
}

void Backend::encodeClip(const QString &outPath, double start, double end, int scaleHeight,
                         const QString &verb, std::function<void(const QString &)> done) {
    const QString ffmpegBin = ffmpeg::toolPath("ffmpeg");
    if (ffmpegBin.isEmpty()) {
        done(QStringLiteral("`ffmpeg` was not found on your PATH."));
        return;
    }

    setBusy(true);
    setStatus(QStringLiteral("%1 0%").arg(verb));

    // Encode to a sibling temp file and atomically replace the target only after
    // success, so failed/cancelled exports preserve any existing file.
    const QString tmpPath = outPath + QStringLiteral(".omacut-part.mp4");
    QFile::remove(tmpPath);
    const QStringList args = ffmpeg::trimArgs(m_path, tmpPath, start, end, scaleHeight);

    auto *proc = new QProcess(this);
    m_encoder = proc;
    auto completed = std::make_shared<bool>(false);
    const auto finish = [proc, tmpPath, completed, done](const QString &error) {
        *completed = true;
        proc->deleteLater();
        if (!error.isEmpty())
            QFile::remove(tmpPath);
        done(error);
    };

    // ffmpeg -progress writes key=value blocks to stdout as it encodes;
    // out_time_us against the clip length gives the percentage.
    const double clipLen = end - start;
    auto progressBuf = std::make_shared<QByteArray>();
    connect(proc, &QProcess::readyReadStandardOutput, this,
            [this, proc, progressBuf, clipLen, completed, verb] {
                progressBuf->append(proc->readAllStandardOutput());
                int newline;
                while ((newline = progressBuf->indexOf('\n')) >= 0) {
                    const QByteArray line = progressBuf->left(newline).trimmed();
                    progressBuf->remove(0, newline + 1);
                    if (*completed || !line.startsWith("out_time_us="))
                        continue;
                    bool ok = false;
                    const double outSecs = line.mid(line.indexOf('=') + 1).toLongLong(&ok) / 1e6;
                    if (!ok)
                        continue;
                    const int percent = qBound(0, qRound(outSecs / clipLen * 100.0), 100);
                    setStatus(QStringLiteral("%1 %2%").arg(verb).arg(percent));
                }
            });

    connect(proc, &QProcess::finished, this,
            [proc, outPath, tmpPath, completed, finish](int code, QProcess::ExitStatus exitStatus) {
                if (*completed)
                    return;
                const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
                if (exitStatus != QProcess::NormalExit || code != 0) {
                    finish(err.isEmpty() ? QStringLiteral("ffmpeg trim failed.") : err);
                    return;
                }
                if (!replaceWithTemp(tmpPath, outPath)) {
                    finish(QStringLiteral("Could not write the exported file."));
                    return;
                }
                finish(QString());
            });
    connect(proc, &QProcess::errorOccurred, this,
            [proc, completed, finish](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || *completed)
                    return;
                const QString err = proc->errorString();
                finish(err.isEmpty() ? QStringLiteral("Could not start ffmpeg.") : err);
            });
    proc->start(ffmpegBin, args);
}

QString Backend::uploadDestinationsDir() const {
    const QString dir = uploads::userDestinationsDir();
    const QString home = QDir::homePath();
    return dir.startsWith(home + QLatin1Char('/')) ? QLatin1Char('~') + dir.mid(home.size()) : dir;
}

QString Backend::uploadHistoryPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
        + QStringLiteral("/omacut/uploads.jsonl");
}

void Backend::reloadUploadDestinations() {
    QStringList warnings;
    m_uploadDestinations = uploads::builtInDestinations()
        + uploads::loadDestinations(uploads::userDestinationsDir(), &warnings);
    for (const QString &warning : warnings)
        qWarning("omacut: skipping uploader %s", qPrintable(warning));

    QStringList names;
    for (const sxcu::Destination &destination : m_uploadDestinations)
        names << destination.name;

    // The choice is remembered by name, so it survives uploaders being added
    // or removed around it.
    const QSettings settings(QSettings::UserScope, kSettingsName, kSettingsName);
    const QString saved = settings.value(kUploadDestinationKey).toString();
    const int index = qMax(0, names.indexOf(saved));
    if (names != m_uploadDestinationNames || index != m_uploadDestination) {
        m_uploadDestinationNames = names;
        m_uploadDestination = index;
        emit uploadDestinationsChanged();
    }
}

void Backend::setUploadDestination(int index) {
    if (index < 0 || index >= m_uploadDestinations.size() || index == m_uploadDestination)
        return;
    m_uploadDestination = index;
    QSettings settings(QSettings::UserScope, kSettingsName, kSettingsName);
    settings.setValue(kUploadDestinationKey, m_uploadDestinations.at(index).name);
    emit uploadDestinationsChanged();
}

QList<int> Backend::uploadHeights() const {
    return m_info.ok ? exportHeights(m_info.width, m_info.height) : QList<int>();
}

void Backend::uploadClip(double start, double end, int scaleHeight) {
    if (m_path.isEmpty() || !m_info.ok || m_busy)
        return;

    if (end - start <= 0.0) {
        emit uploadFailed(QStringLiteral("The selected clip has no length."));
        return;
    }
    if (m_uploadDestination < 0 || m_uploadDestination >= m_uploadDestinations.size()) {
        emit uploadFailed(QStringLiteral("No upload destination is set up."));
        return;
    }

    // The trim is encoded into a private temp dir under the same name an
    // export would suggest, since hosts show that name to whoever opens it.
    m_uploadDir = std::make_unique<QTemporaryDir>(QDir::tempPath()
                                                  + QStringLiteral("/omacut-upload-XXXXXX"));
    if (!m_uploadDir->isValid()) {
        m_uploadDir.reset();
        emit uploadFailed(QStringLiteral("Could not create a temporary file for the upload."));
        return;
    }
    const QString clipPath = m_uploadDir->filePath(suggestedExportUrl().fileName());
    const sxcu::Destination destination = m_uploadDestinations.at(m_uploadDestination);
    m_uploading = true;
    m_uploadCancelled = false;
    m_uploadSource = m_path;
    m_uploadStart = start;
    m_uploadEnd = end;
    emit uploadingChanged();

    encodeClip(clipPath, start, end, scaleHeight, QStringLiteral("Encoding"),
               [this, clipPath, destination](const QString &error) {
                   if (m_uploadCancelled || !error.isEmpty()) {
                       finishUpload();
                       emit uploadFailed(m_uploadCancelled ? QStringLiteral("Upload cancelled.")
                                                           : error);
                       return;
                   }
                   m_uploadDestinationName = destination.name;
                   setStatus(QStringLiteral("Uploading 0%"));
                   m_uploader->upload(destination, clipPath);
               });
}

void Backend::cancelUpload() {
    if (!m_uploading)
        return;
    m_uploadCancelled = true;
    if (m_uploader->busy())
        m_uploader->cancel();
    else if (m_encoder)
        m_encoder->kill();
}

void Backend::finishUpload() {
    m_uploadDir.reset();
    setBusy(false);
    setStatus(QString());
    if (m_uploading) {
        m_uploading = false;
        emit uploadingChanged();
    }
}

void Backend::appendUploadHistory(const QString &url, const QString &thumbnailUrl,
                                  const QString &deletionUrl) const {
    // One JSON object per line, so a deletion link is never lost to the
    // clipboard being overwritten.
    QJsonObject entry{
        {QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODate)},
        {QStringLiteral("destination"), m_uploadDestinationName},
        {QStringLiteral("source"), m_uploadSource},
        {QStringLiteral("start"), m_uploadStart},
        {QStringLiteral("end"), m_uploadEnd},
        {QStringLiteral("url"), url},
    };
    if (!thumbnailUrl.isEmpty())
        entry.insert(QStringLiteral("thumbnail"), thumbnailUrl);
    if (!deletionUrl.isEmpty())
        entry.insert(QStringLiteral("deletion"), deletionUrl);

    const QString path = uploadHistoryPath();
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
}

void Backend::copyToClipboard(const QString &text) {
    // A Wayland selection dies with the app that owns it, and omacut is often
    // closed right after sharing; wl-copy keeps serving it in the background.
    if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
        const QString wlCopy = QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
        if (!wlCopy.isEmpty() && QProcess::startDetached(wlCopy, {QStringLiteral("--"), text}))
            return;
    }
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}
