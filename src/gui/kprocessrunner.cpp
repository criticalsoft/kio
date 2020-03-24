/* This file is part of the KDE libraries
    Copyright (c) 2020 David Faure <faure@kde.org>

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License or ( at
    your option ) version 3 or, at the discretion of KDE e.V. ( which shall
    act as a proxy as in section 14 of the GPLv3 ), any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include "kprocessrunner_p.h"

#include "kiogui_debug.h"
#include "config-kiogui.h"

#include "desktopexecparser.h"
#include "krecentdocument.h"
#include <KDesktopFile>
#include <KLocalizedString>
#include <KWindowSystem>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStandardPaths>

static int s_instanceCount = 0; // for the unittest

KProcessRunner::KProcessRunner(const KService::Ptr &service, const QList<QUrl> &urls,
                               KIO::ApplicationLauncherJob::RunFlags flags, const QString &suggestedFileName, const QByteArray &asn)
    : m_process{new KProcess},
      m_executable(KIO::DesktopExecParser::executablePath(service->exec()))
{
    ++s_instanceCount;
    KIO::DesktopExecParser execParser(*service, urls);

    const QString realExecutable = execParser.resultingArguments().at(0);
    if (!QFileInfo::exists(realExecutable) && QStandardPaths::findExecutable(realExecutable).isEmpty()) {
        emitDelayedError(i18n("Could not find the program '%1'", realExecutable));
        return;
    }

    execParser.setUrlsAreTempFiles(flags & KIO::ApplicationLauncherJob::DeleteTemporaryFiles);
    execParser.setSuggestedFileName(suggestedFileName);
    const QStringList args = execParser.resultingArguments();
    if (args.isEmpty()) {
        emitDelayedError(i18n("Error processing Exec field in %1", service->entryPath()));
        return;
    }
    //qDebug() << "KProcess args=" << args;
    *m_process << args;

    enum DiscreteGpuCheck { NotChecked, Present, Absent };
    static DiscreteGpuCheck s_gpuCheck = NotChecked;

    if (service->runOnDiscreteGpu() && s_gpuCheck == NotChecked) {
        // Check whether we have a discrete gpu
        bool hasDiscreteGpu = false;
        QDBusInterface iface(QStringLiteral("org.kde.Solid.PowerManagement"),
                             QStringLiteral("/org/kde/Solid/PowerManagement"),
                             QStringLiteral("org.kde.Solid.PowerManagement"),
                             QDBusConnection::sessionBus());
        if (iface.isValid()) {
            QDBusReply<bool> reply = iface.call(QStringLiteral("hasDualGpu"));
            if (reply.isValid()) {
                hasDiscreteGpu = reply.value();
            }
        }

        s_gpuCheck = hasDiscreteGpu ? Present : Absent;
    }

    if (service->runOnDiscreteGpu() && s_gpuCheck == Present) {
        m_process->setEnv(QStringLiteral("DRI_PRIME"), QStringLiteral("1"));
    }

    QString workingDir(service->workingDirectory());
    if (workingDir.isEmpty() && !urls.isEmpty() && urls.first().isLocalFile()) {
        workingDir = urls.first().adjusted(QUrl::RemoveFilename).toLocalFile();
    }
    m_process->setWorkingDirectory(workingDir);

    if ((flags & KIO::ApplicationLauncherJob::DeleteTemporaryFiles) == 0) {
        // Remember we opened those urls, for the "recent documents" menu in kicker
        for (const QUrl &url : urls) {
            KRecentDocument::add(url, service->desktopEntryName());
        }
    }

    init(service, service->name(), service->icon(), asn);
}

KProcessRunner::KProcessRunner(const QString &cmd, const QString &desktopName, const QString &execName, const QString &iconName, const QByteArray &asn, const QString &workingDirectory)
    : m_process{new KProcess},
      m_executable(execName)
{
    ++s_instanceCount;
    m_process->setShellCommand(cmd);
    if (!workingDirectory.isEmpty()) {
        m_process->setWorkingDirectory(workingDirectory);
    }
    if (!desktopName.isEmpty()) {
        KService::Ptr service = KService::serviceByDesktopName(desktopName);
        if (service) {
            if (m_executable.isEmpty()) {
                m_executable = KIO::DesktopExecParser::executablePath(service->exec());
            }
            init(service, service->name(), service->icon(), asn);
            return;
        }
    }
    init(KService::Ptr(), execName /*user-visible name*/, iconName, asn);
}

void KProcessRunner::init(const KService::Ptr &service, const QString &userVisibleName, const QString &iconName, const QByteArray &asn)
{
    if (service && !service->entryPath().isEmpty()
            && !KDesktopFile::isAuthorizedDesktopFile(service->entryPath())) {
        qCWarning(KIO_GUI) << "No authorization to execute " << service->entryPath();
        emitDelayedError(i18n("You are not authorized to execute this file."));
        return;
    }

#if HAVE_X11
    static bool isX11 = QGuiApplication::platformName() == QLatin1String("xcb");
    if (isX11) {
        bool silent;
        QByteArray wmclass;
        const bool startup_notify = (asn != "0" && KIOGuiPrivate::checkStartupNotify(service.data(), &silent, &wmclass));
        if (startup_notify) {
            m_startupId.initId(asn);
            m_startupId.setupStartupEnv();
            KStartupInfoData data;
            data.setHostname();
            // When it comes from a desktop file, m_executable can be a full shell command, so <bin> here is not 100% reliable.
            // E.g. it could be "cd", which isn't an existing binary. It's just a heuristic anyway.
            const QString bin = KIO::DesktopExecParser::executableName(m_executable);
            data.setBin(bin);
            if (!userVisibleName.isEmpty()) {
                data.setName(userVisibleName);
            } else if (service && !service->name().isEmpty()) {
                data.setName(service->name());
            }
            data.setDescription(i18n("Launching %1", data.name()));
            if (!iconName.isEmpty()) {
                data.setIcon(iconName);
            } else if (service && !service->icon().isEmpty()) {
                data.setIcon(service->icon());
            }
            if (!wmclass.isEmpty()) {
                data.setWMClass(wmclass);
            }
            if (silent) {
                data.setSilent(KStartupInfoData::Yes);
            }
            data.setDesktop(KWindowSystem::currentDesktop());
            if (service && !service->entryPath().isEmpty()) {
                data.setApplicationId(service->entryPath());
            }
            KStartupInfo::sendStartup(m_startupId, data);
        }
    }
#else
    Q_UNUSED(bin);
    Q_UNUSED(userVisibleName);
    Q_UNUSED(iconName);
#endif
    startProcess();
}

void KProcessRunner::startProcess()
{
    connect(m_process.get(), QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
            this, &KProcessRunner::slotProcessExited);
    connect(m_process.get(), &QProcess::started,
            this, &KProcessRunner::slotProcessStarted, Qt::QueuedConnection);
    connect(m_process.get(), &QProcess::errorOccurred,
            this, &KProcessRunner::slotProcessError);

    m_process->start();
}

bool KProcessRunner::waitForStarted()
{
    return m_process->waitForStarted();
}

void KProcessRunner::slotProcessError(QProcess::ProcessError errorCode)
{
    // E.g. the process crashed.
    // This is unlikely to happen while the ApplicationLauncherJob is still connected to the KProcessRunner.
    // So the emit does nothing, this is really just for debugging.
    qCDebug(KIO_GUI) << m_executable << "error=" << errorCode << m_process->errorString();
    Q_EMIT error(m_process->errorString());
}

void KProcessRunner::slotProcessStarted()
{
    m_pid = m_process->processId();

#if HAVE_X11
    if (!m_startupId.isNull() && m_pid) {
        KStartupInfoData data;
        data.addPid(m_pid);
        KStartupInfo::sendChange(m_startupId, data);
        KStartupInfo::resetStartupEnv();
    }
#endif
    emit processStarted();
}

KProcessRunner::~KProcessRunner()
{
    // This destructor deletes m_process, since it's a unique_ptr.
    --s_instanceCount;
}

int KProcessRunner::instanceCount()
{
    return s_instanceCount;
}

qint64 KProcessRunner::pid() const
{
    return m_pid;
}

void KProcessRunner::terminateStartupNotification()
{
#if HAVE_X11
    if (!m_startupId.isNull()) {
        KStartupInfoData data;
        data.addPid(m_pid); // announce this pid for the startup notification has finished
        data.setHostname();
        KStartupInfo::sendFinish(m_startupId, data);
    }
#endif
}

void KProcessRunner::emitDelayedError(const QString &errorMsg)
{
    terminateStartupNotification();
    // Use delayed invocation so the caller has time to connect to the signal
    QMetaObject::invokeMethod(this, [this, errorMsg]() {
        emit error(errorMsg);
        deleteLater();
    }, Qt::QueuedConnection);
}

void KProcessRunner::slotProcessExited(int exitCode, QProcess::ExitStatus exitStatus)
{
    qCDebug(KIO_GUI) << m_executable << "exitCode=" << exitCode << "exitStatus=" << exitStatus;
    terminateStartupNotification();
    deleteLater();
}

// This code is also used in klauncher (and KRun).
bool KIOGuiPrivate::checkStartupNotify(const KService *service, bool *silent_arg, QByteArray *wmclass_arg)
{
    bool silent = false;
    QByteArray wmclass;
    if (service && service->property(QStringLiteral("StartupNotify")).isValid()) {
        silent = !service->property(QStringLiteral("StartupNotify")).toBool();
        wmclass = service->property(QStringLiteral("StartupWMClass")).toString().toLatin1();
    } else if (service && service->property(QStringLiteral("X-KDE-StartupNotify")).isValid()) {
        silent = !service->property(QStringLiteral("X-KDE-StartupNotify")).toBool();
        wmclass = service->property(QStringLiteral("X-KDE-WMClass")).toString().toLatin1();
    } else { // non-compliant app
        if (service) {
            if (service->isApplication()) { // doesn't have .desktop entries needed, start as non-compliant
                wmclass = "0"; // krazy:exclude=doublequote_chars
            } else {
                return false; // no startup notification at all
            }
        } else {
#if 0
            // Create startup notification even for apps for which there shouldn't be any,
            // just without any visual feedback. This will ensure they'll be positioned on the proper
            // virtual desktop, and will get user timestamp from the ASN ID.
            wmclass = '0';
            silent = true;
#else   // That unfortunately doesn't work, when the launched non-compliant application
            // launches another one that is compliant and there is any delay inbetween (bnc:#343359)
            return false;
#endif
        }
    }
    if (silent_arg) {
        *silent_arg = silent;
    }
    if (wmclass_arg) {
        *wmclass_arg = wmclass;
    }
    return true;
}