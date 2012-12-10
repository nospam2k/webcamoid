/* Webcamod, webcam capture plasmoid.
 * Copyright (C) 2011-2012  Gonzalo Exequiel Pedone
 *
 * Webcamod is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Webcamod is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Webcamod. If not, see <http://www.gnu.org/licenses/>.
 *
 * Email     : hipersayan DOT x AT gmail DOT com
 * Web-Site 1: http://github.com/hipersayanX/Webcamoid
 * Web-Site 2: http://kde-apps.org/content/show.php/Webcamoid?content=144796
 */

// http://gstreamer.freedesktop.org/data/doc/gstreamer/head/qt-gstreamer/html/index.html
// http://gstreamer.freedesktop.org/documentation/
// http://api.kde.org/
// http://extragear.kde.org/apps/kipi/
// LD_PRELOAD='./libWebcamoid.so' ./Webcamoid

#include <sys/ioctl.h>
#include <KSharedConfig>
#include <KConfigGroup>
#include <gst/app/gstappsink.h>
#include <linux/videodev2.h>

#include "v4l2tools.h"

V4L2Tools::V4L2Tools(bool watchDevices, QObject *parent): QObject(parent)
{
    gst_init(NULL, NULL);

    this->m_appEnvironment = new AppEnvironment(this);

    QObject::connect(QCoreApplication::instance(),
                     SIGNAL(aboutToQuit()),
                     this,
                     SLOT(aboutToQuit()));

    this->resetPlaying();
    this->resetRecordAudio();
    this->resetRecording();
    this->resetVideoRecordFormats();
    this->resetCurDevName();
    this->resetStreams();

    this->m_curOutVidFmt = "webm";

    this->m_mainPipeline = gst_pipeline_new("MainPipeline");

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(this->m_mainPipeline));
    this->m_busWatchId = gst_bus_add_watch(bus, V4L2Tools::busMessage, this);
    gst_object_unref(GST_OBJECT(bus));

    QString captureHash = this->hashFromName("capture");
    QString mainBinDescription = QString("tee name=CaptureTee ! "
                                         "tee name=EffectsTee ! "
                                         "ffmpegcolorspace ! "
                                         "ffenc_bmp ! "
                                         "appsink name=%1 emit-signals=true").arg(captureHash);

    GError *error = NULL;

    this->m_mainBin = gst_parse_bin_from_description(mainBinDescription.toUtf8().constData(),
                                                     FALSE,
                                                     &error);

    g_object_set(GST_OBJECT(this->m_mainBin), "name", "mainBin", NULL);

    GstElement *capture = gst_bin_get_by_name(GST_BIN(this->m_mainBin), captureHash.toUtf8().constData());
    g_signal_connect(capture, "new-buffer",  G_CALLBACK(V4L2Tools::readFrame), this);
    gst_object_unref(GST_OBJECT(capture));

    gst_bin_add(GST_BIN(this->m_mainPipeline), this->m_mainBin);

    this->m_captureDevice = NULL;
    this->m_effectsBin = NULL;
    this->m_effectsPreviewBin = NULL;

    if (watchDevices)
    {
        this->m_fsWatcher = new QFileSystemWatcher(QStringList() << "/dev", this);

        QObject::connect(this->m_fsWatcher,
                         SIGNAL(directoryChanged(const QString &)),
                         this,
                         SLOT(onDirectoryChanged(const QString &)));
    }

    this->loadConfigs();
}

V4L2Tools::~V4L2Tools()
{
    this->stopCurrentDevice();
    this->saveConfigs();

    g_source_remove(this->m_busWatchId);
    gst_object_unref(GST_OBJECT(this->m_mainPipeline));
}

bool V4L2Tools::playing()
{
    return this->m_playing;
}

bool V4L2Tools::recordAudio()
{
    return this->m_recordAudio;
}

bool V4L2Tools::recording()
{
    return this->m_recording;
}

QVariantList V4L2Tools::videoRecordFormats()
{
    return this->m_videoRecordFormats;
}

QString V4L2Tools::curDevName()
{
    return this->m_curDevName;
}

QVariantList V4L2Tools::streams()
{
    return this->m_streams;
}

QString V4L2Tools::fcc2s(uint val)
{
    QString s = "";

    s += QChar(val & 0xff);
    s += QChar((val >> 8) & 0xff);
    s += QChar((val >> 16) & 0xff);
    s += QChar((val >> 24) & 0xff);

    return s;
}

QVariantList V4L2Tools::videoFormats(QString dev_name)
{
    QFile device(dev_name);
    QVariantList formats;

    if (!device.open(QIODevice::ReadWrite | QIODevice::Unbuffered))
        return formats;

    QList<v4l2_buf_type> bufType;

    bufType << V4L2_BUF_TYPE_VIDEO_CAPTURE
            << V4L2_BUF_TYPE_VIDEO_OUTPUT
            << V4L2_BUF_TYPE_VIDEO_OVERLAY;

    foreach (v4l2_buf_type type, bufType)
    {
        v4l2_fmtdesc fmt;
        fmt.index = 0;
        fmt.type = type;

        while (ioctl(device.handle(), VIDIOC_ENUM_FMT, &fmt) >= 0)
        {
            v4l2_frmsizeenum frmsize;
            frmsize.pixel_format = fmt.pixelformat;
            frmsize.index = 0;

            while (ioctl(device.handle(),
                         VIDIOC_ENUM_FRAMESIZES,
                         &frmsize) >= 0)
            {
                if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                {
                    QVariantList format;

                    format << frmsize.discrete.width
                           << frmsize.discrete.height
                           << fmt.pixelformat;

                    formats << QVariant(format);
                }

                frmsize.index++;
            }

            fmt.index++;
        }
    }

    device.close();

    return formats;
}

QVariantList V4L2Tools::currentVideoFormat(QString dev_name)
{
    QFile device(dev_name);
    QVariantList format;

    if (!device.open(QIODevice::ReadWrite | QIODevice::Unbuffered))
        return format;

    v4l2_format fmt;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(device.handle(), VIDIOC_G_FMT, &fmt) >= 0)
        format << fmt.fmt.pix.width
               << fmt.fmt.pix.height
               << fmt.fmt.pix.pixelformat;

    device.close();

    return format;
}

bool V4L2Tools::setVideoFormat(QString dev_name, QVariantList videoFormat)
{
    QFile device(dev_name);

    if (!device.open(QIODevice::ReadWrite | QIODevice::Unbuffered))
        return false;

    v4l2_format fmt;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(device.handle(), VIDIOC_G_FMT, &fmt) >= 0)
    {
        fmt.fmt.pix.width = videoFormat.at(0).toUInt();
        fmt.fmt.pix.height = videoFormat.at(1).toUInt();
        fmt.fmt.pix.pixelformat = videoFormat.at(2).toUInt();

        if (ioctl(device.handle(), VIDIOC_S_FMT, &fmt) < 0)
        {
            device.close();
            this->startDevice(dev_name, videoFormat);

            return true;
        }
    }

    device.close();

    return true;
}

QVariantList V4L2Tools::captureDevices()
{
    QVariantList webcamsDevices;
    QDir devicesDir("/dev");

    QStringList devices = devicesDir.entryList(QStringList() << "video*",
                                               QDir::System |
                                               QDir::Readable |
                                               QDir::Writable |
                                               QDir::NoSymLinks |
                                               QDir::NoDotAndDotDot |
                                               QDir::CaseSensitive,
                                               QDir::Name);

    QFile device;
    v4l2_capability capability;

    foreach (QString devicePath, devices)
    {
        device.setFileName(devicesDir.absoluteFilePath(devicePath));

        if (device.open(QIODevice::ReadWrite))
        {
            ioctl(device.handle(), VIDIOC_QUERYCAP, &capability);

            if (capability.capabilities & V4L2_CAP_VIDEO_CAPTURE)
            {
                QVariantList cap;

                cap << device.fileName()
                    << QString((const char *) capability.card)
                    << StreamTypeWebcam;

                webcamsDevices << QVariant(cap);
            }

            device.close();
        }
    }

    this->m_webcams = webcamsDevices;

    QVariantList desktopDevice = QVariantList() << "desktop"
                                                << this->tr("Desktop")
                                                << StreamTypeDesktop;

    QVariantList allDevices = webcamsDevices +
                              this->m_streams +
                              QVariantList() << QVariant(desktopDevice);

    return allDevices;
}

QVariantList V4L2Tools::listControls(QString dev_name)
{
    v4l2_queryctrl queryctrl;
    queryctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    QVariantList controls;

    QFile device(dev_name);

    if (!device.open(QIODevice::ReadWrite | QIODevice::Unbuffered))
        return controls;

    while (ioctl(device.handle(), VIDIOC_QUERYCTRL, &queryctrl) == 0)
    {
        QVariantList control = this->queryControl(device.handle(), &queryctrl);

        if (!control.isEmpty())
            controls << QVariant(control);

        queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }

    if (queryctrl.id != V4L2_CTRL_FLAG_NEXT_CTRL)
    {
        device.close();

        return controls;
    }

    for (int id = V4L2_CID_USER_BASE; id < V4L2_CID_LASTP1; id++)
    {
        queryctrl.id = id;

        if (ioctl(device.handle(), VIDIOC_QUERYCTRL, &queryctrl) == 0)
        {
            QVariantList control = this->queryControl(device.handle(), &queryctrl);

            if (!control.isEmpty())
                controls << QVariant(control);
        }
    }

    for (queryctrl.id = V4L2_CID_PRIVATE_BASE; ioctl(device.handle(), VIDIOC_QUERYCTRL, &queryctrl) == 0; queryctrl.id++)
    {
        QVariantList control = this->queryControl(device.handle(), &queryctrl);

        if (!control.isEmpty())
            controls << QVariant(control);
    }

    device.close();

    return controls;
}

bool V4L2Tools::setControls(QString dev_name, QMap<QString, uint> controls)
{
    QFile device(dev_name);

    if (!device.open(QIODevice::ReadWrite | QIODevice::Unbuffered))
        return false;

    QMap<QString, uint> ctrl2id = this->findControls(device.handle());
    std::vector<v4l2_ext_control> mpeg_ctrls;
    std::vector<v4l2_ext_control> user_ctrls;

    foreach (QString control, controls.keys())
    {
        v4l2_ext_control ctrl;
        ctrl.id = ctrl2id[control];
        ctrl.value = controls[control];

        if (V4L2_CTRL_ID2CLASS(ctrl.id) == V4L2_CTRL_CLASS_MPEG)
            mpeg_ctrls.push_back(ctrl);
        else
            user_ctrls.push_back(ctrl);
    }

    foreach (v4l2_ext_control user_ctrl, user_ctrls)
    {
        v4l2_control ctrl;
        ctrl.id = user_ctrl.id;
        ctrl.value = user_ctrl.value;
        ioctl(device.handle(), VIDIOC_S_CTRL, &ctrl);
    }

    if (!mpeg_ctrls.empty())
    {
        v4l2_ext_controls ctrls;
        ctrls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
        ctrls.count = mpeg_ctrls.size();
        ctrls.controls = &mpeg_ctrls[0];
        ioctl(device.handle(), VIDIOC_S_EXT_CTRLS, &ctrls);
    }

    device.close();

    return true;
}

QVariantMap V4L2Tools::featuresMatrix()
{
    QVariantMap features;
    QStringList availableElements;

    GList *headElement = gst_registry_get_feature_list(gst_registry_get_default(), GST_TYPE_ELEMENT_FACTORY);
    GList *element = headElement;

    while (element)
    {
        GstPluginFeature *pluginFeature = GST_PLUGIN_FEATURE(element->data);
        availableElements << QString(gst_plugin_feature_get_name(pluginFeature));
        element = g_list_next(element);
    }

    gst_plugin_feature_list_free(headElement);

    QStringList elements;
    bool libAvailable = true;

    elements << "appsink"
             << "appsrc"
             << "filesink"
             << "queue"
             << "tee"
             << "uridecodebin";

    // GStreamer Core:
    foreach (QString element, elements)
        if (!availableElements.contains(element))
        {
            libAvailable = false;

            break;
        }

    features["gst-core"] = QVariantList() << libAvailable
                                          << "GStreamer Core"
                                          << this->tr("Basic functionality.");

    elements.clear();
    libAvailable = true;

    elements << "alsasrc"
             << "audioconvert"
             << "decodebin"
             << "ffmpegcolorspace"
             << "theoraenc"
             << "videoscale"
             << "vorbisenc";

    // GStreamer Base Plugins:
    foreach (QString element, elements)
        if (!availableElements.contains(element))
        {
            libAvailable = false;

            break;
        }

    features["gst-base-plugins"] = QVariantList() << libAvailable
                                                  << "GStreamer Base Plugins"
                                                  << this->tr("Transcoding and audio source.");

    elements.clear();
    libAvailable = true;

    elements << "agingtv"
             << "dicetv"
             << "edgetv"
             << "optv"
             << "quarktv"
             << "radioactv"
             << "revtv"
             << "rippletv"
             << "shagadelictv"
             << "streaktv"
             << "v4l2src"
             << "vertigotv"
             << "videobalance"
             << "videoflip"
             << "warptv"
             << "ximagesrc";

    // GStreamer Good Plugins:
    foreach (QString element, elements)
        if (!availableElements.contains(element))
        {
            libAvailable = false;

            break;
        }

    features["gst-good-plugins"] = QVariantList() << libAvailable
                                                  << "GStreamer Good Plugins"
                                                  << this->tr("Basic sources and effects.");

    elements.clear();
    libAvailable = true;

    elements << "bulge"
             << "burn"
             << "chromium"
             << "coloreffects"
             << "exclusion"
             << "fisheye"
             << "kaleidoscope"
             << "marble"
             << "mirror"
             << "pinch"
             << "solarize"
             << "sphere"
             << "stretch"
             << "tunnel"
             << "twirl"
             << "vp8enc"
             << "waterripple";

    // GStreamer Bad Plugins:
    foreach (QString element, elements)
        if (!availableElements.contains(element))
        {
            libAvailable = false;

            break;
        }

    features["gst-bad-plugins"] = QVariantList() << libAvailable
                                                 << "GStreamer Bad Plugins"
                                                 << this->tr("Effects and some codecs.");

    elements.clear();
    libAvailable = true;

    elements << "frei0r-filter-cartoon"
             << "frei0r-filter-delaygrab"
             << "frei0r-filter-distort0r"
             << "frei0r-filter-equaliz0r"
             << "frei0r-filter-hqdn3d"
             << "frei0r-filter-invert0r"
             << "frei0r-filter-nervous"
             << "frei0r-filter-pixeliz0r"
             << "frei0r-filter-primaries"
             << "frei0r-filter-sobel"
             << "frei0r-filter-sop-sat"
             << "frei0r-filter-threelay0r"
             << "frei0r-filter-twolay0r";

    // frei0r Plugins:
    foreach (QString element, elements)
        if (!availableElements.contains(element))
        {
            libAvailable = false;

            break;
        }

    features["frei0r-plugins"] = QVariantList() << libAvailable
                                                << "frei0r Plugins"
                                                << this->tr("Extra effects.");

    elements.clear();
    libAvailable = true;

    elements << "ffdec_bmp"
             << "ffenc_bmp";

    // GStreamer FFmpeg Plugins:
    foreach (QString element, elements)
        if (!availableElements.contains(element))
        {
            libAvailable = false;

            break;
        }

    features["gst-ffmpeg"] = QVariantList() << libAvailable
                                            << "GStreamer FFmpeg Plugins"
                                            << this->tr("Basic functionality.");

    return features;
}

QMap<QString, QString> V4L2Tools::availableEffects()
{
    QVariantMap features = this->featuresMatrix();
    QMap<QString, QString> effects;

    if (features["gst-good-plugins"].toList().at(0).toBool())
    {
        effects["agingtv"] = this->tr("Old");
        effects["dicetv"] = this->tr("Dices");
        effects["edgetv"] = this->tr("Edges");
        effects["optv"] = this->tr("Hypnotic");
        effects["quarktv"] = this->tr("Quark");
        effects["radioactv"] = this->tr("Radioactive");
        effects["revtv"] = this->tr("Scan Lines");
        effects["rippletv"] = this->tr("Ripple");
        effects["shagadelictv"] = this->tr("Psychedelic");
        effects["streaktv"] = this->tr("Streak");
        effects["vertigotv"] = this->tr("Vertigo");
        effects["videobalance saturation=1.5 hue=-0.5"] = this->tr("Hulk");
        effects["videobalance saturation=1.5 hue=+0.5"] = this->tr("Mauve");
        effects["videobalance saturation=0"] = this->tr("Noir");
        effects["videobalance saturation=2"] = this->tr("Saturation");
        effects["videoflip method=clockwise"] = this->tr("Rotate Right");
        effects["videoflip method=rotate-180"] = this->tr("Rotate 180");
        effects["videoflip method=counterclockwise"] = this->tr("Rotate Left");
        effects["videoflip method=horizontal-flip"] = this->tr("Flip horizontally");
        effects["videoflip method=vertical-flip"] = this->tr("Flip vertically");
        effects["videoflip method=upper-left-diagonal"] = this->tr("Flip Top Left");
        effects["videoflip method=upper-right-diagonal"] = this->tr("Flip Top Right");
        effects["warptv"] = this->tr("Warp");
    }

    if (features["gst-bad-plugins"].toList().at(0).toBool())
    {
        effects["bulge"] = this->tr("Bulge");
        effects["burn"] = this->tr("Burn");
        effects["chromium"] = this->tr("Chromium");
        effects["coloreffects preset=heat"] = this->tr("Heat");
        effects["coloreffects preset=sepia"] = this->tr("Sepia");
        effects["coloreffects preset=xray"] = this->tr("X-Ray");
        effects["coloreffects preset=xpro"] = this->tr("X-Pro");
        effects["exclusion"] = this->tr("Exclusion");
        effects["fisheye"] = this->tr("Fish Eye");
        effects["kaleidoscope"] = this->tr("Kaleidoscope");
        effects["marble"] = this->tr("Marble");
        effects["mirror"] = this->tr("Mirror");
        effects["pinch"] = this->tr("Pinch");
        effects["solarize"] = this->tr("Solarize");
        effects["sphere"] = this->tr("Sphere");
        effects["sphere"] = this->tr("Square");
        effects["stretch"] = this->tr("Stretch");
        effects["tunnel"] = this->tr("Tunnel");
        effects["twirl"] = this->tr("Twirl");
        effects["waterripple"] = this->tr("Water Ripple");
    }

    if (features["frei0r-plugins"].toList().at(0).toBool())
    {
        effects["frei0r-filter-cartoon"] = this->tr("Cartoon");
        effects["frei0r-filter-delaygrab"] = this->tr("Past");
        effects["frei0r-filter-distort0r"] = this->tr("Distort");
        effects["frei0r-filter-equaliz0r"] = this->tr("Equalize");
        effects["frei0r-filter-hqdn3d spatial=0.5 temporal=1.0"] = this->tr("Drugs");
        effects["frei0r-filter-invert0r"] = this->tr("Invert");
        effects["frei0r-filter-nervous"] = this->tr("Nervous");
        effects["frei0r-filter-pixeliz0r"] = this->tr("Pixelate");
        effects["frei0r-filter-primaries"] = this->tr("Primary Colors");
        effects["frei0r-filter-sobel"] = this->tr("Sobel");
        effects["frei0r-filter-sop-sat"] = this->tr("Crazy Colors");
        effects["frei0r-filter-threelay0r"] = this->tr("The Godfather");
        effects["frei0r-filter-twolay0r"] = this->tr("Che Guevara");
    }

    return effects;
}

QStringList V4L2Tools::currentEffects()
{
    return this->m_effects;
}

QStringList V4L2Tools::bestVideoRecordFormat(QString fileName)
{
    QString ext = QFileInfo(fileName).completeSuffix();

    if (ext.isEmpty())
        return QStringList();

    foreach (QVariant format, this->m_videoRecordFormats)
        foreach (QString s, format.toList().at(0).toString().split(",", QString::SkipEmptyParts))
            if (s.toLower().trimmed() == ext)
                return format.toStringList();

    return QStringList();
}

QVariantList V4L2Tools::queryControl(int dev_fd, v4l2_queryctrl *queryctrl)
{
    if (queryctrl->flags & V4L2_CTRL_FLAG_DISABLED)
        return QVariantList();

    if (queryctrl->type == V4L2_CTRL_TYPE_CTRL_CLASS)
        return QVariantList();

    v4l2_ext_control ext_ctrl;
    ext_ctrl.id = queryctrl->id;

    v4l2_ext_controls ctrls;
    ctrls.ctrl_class = V4L2_CTRL_ID2CLASS(queryctrl->id);
    ctrls.count = 1;
    ctrls.controls = &ext_ctrl;

    if (V4L2_CTRL_ID2CLASS(queryctrl->id) != V4L2_CTRL_CLASS_USER &&
        queryctrl->id < V4L2_CID_PRIVATE_BASE)
    {
        if (ioctl(dev_fd, VIDIOC_G_EXT_CTRLS, &ctrls))
            return QVariantList();
    }
    else
    {
        v4l2_control ctrl;
        ctrl.id = queryctrl->id;

        if (ioctl(dev_fd, VIDIOC_G_CTRL, &ctrl))
            return QVariantList();

        ext_ctrl.value = ctrl.value;
    }

    v4l2_querymenu qmenu;
    qmenu.id = queryctrl->id;
    QStringList menu;

    if (queryctrl->type == V4L2_CTRL_TYPE_MENU)
        for (int i = 0; i < queryctrl->maximum + 1; i++)
        {
            qmenu.index = i;

            if (ioctl(dev_fd, VIDIOC_QUERYMENU, &qmenu))
                continue;

            menu << QString((const char *) qmenu.name);
        }

    return QVariantList() << QString((const char *) queryctrl->name)
                          << queryctrl->type
                          << queryctrl->minimum
                          << queryctrl->maximum
                          << queryctrl->step
                          << queryctrl->default_value
                          << ext_ctrl.value
                          << menu;
}

QMap<QString, uint> V4L2Tools::findControls(int dev_fd)
{
    v4l2_queryctrl qctrl;
    qctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    QMap<QString, uint> controls;

    while (ioctl(dev_fd, VIDIOC_QUERYCTRL, &qctrl) == 0)
    {
        if (qctrl.type != V4L2_CTRL_TYPE_CTRL_CLASS &&
            !(qctrl.flags & V4L2_CTRL_FLAG_DISABLED))
            controls[QString((const char *) qctrl.name)] = qctrl.id;

        qctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }

    if (qctrl.id != V4L2_CTRL_FLAG_NEXT_CTRL)
        return controls;

    for (int id = V4L2_CID_USER_BASE; id < V4L2_CID_LASTP1; id++)
    {
        qctrl.id = id;

        if (ioctl(dev_fd, VIDIOC_QUERYCTRL, &qctrl) == 0 &&
           !(qctrl.flags & V4L2_CTRL_FLAG_DISABLED))
            controls[QString((const char *) qctrl.name)] = qctrl.id;
    }

    qctrl.id = V4L2_CID_PRIVATE_BASE;

    while (ioctl(dev_fd, VIDIOC_QUERYCTRL, &qctrl) == 0)
    {
        if (!(qctrl.flags & V4L2_CTRL_FLAG_DISABLED))
            controls[QString((const char *) qctrl.name)] = qctrl.id;

        qctrl.id++;
    }

    return controls;
}

QString V4L2Tools::hashFromName(QString name)
{
    return QString("x") + name.toUtf8().toHex();
}

QString V4L2Tools::nameFromHash(QString hash)
{
    return QByteArray::fromHex(hash.mid(1).toUtf8());
}

V4L2Tools::StreamType V4L2Tools::deviceType(QString dev_name)
{
    QStringList webcams;

    foreach (QVariant device, this->m_webcams)
        webcams << device.toList().at(0).toString();

    QStringList streams;

    foreach (QVariant device, this->m_streams)
        streams << device.toList().at(0).toString();

    if (webcams.contains(dev_name))
        return StreamTypeWebcam;
    else if (streams.contains(dev_name))
        return StreamTypeURI;
    else if (dev_name == "desktop")
        return StreamTypeDesktop;
    else
        return StreamTypeUnknown;
}

gboolean V4L2Tools::busMessage(GstBus *bus, GstMessage *message, gpointer self)
{
    Q_UNUSED(bus)

    V4L2Tools *v4l2Tools = (V4L2Tools* ) self;

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS)
        gst_element_set_state(v4l2Tools->m_mainPipeline, GST_STATE_NULL);
    else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
    {
        GError *err;
        gchar *debug;

        gst_message_parse_error (message, &err, &debug);
        qDebug() << "Error: " << err->message;
        g_error_free (err);
        g_free (debug);

        gst_element_set_state(v4l2Tools->m_mainPipeline, GST_STATE_NULL);
    }
    else
    {
    }

    return TRUE;
}

void V4L2Tools::readFrame(GstElement *appsink, gpointer self)
{
    V4L2Tools *v4l2Tools = (V4L2Tools* ) self;

    v4l2Tools->m_mutex.lock();

    gchar *elementName = gst_element_get_name(appsink);
    QString pipename = v4l2Tools->nameFromHash(elementName);
    g_free(elementName);

    GstBuffer *buffer = gst_app_sink_pull_buffer(GST_APP_SINK(appsink));
    QImage frame = QImage::fromData((const uchar *) GST_BUFFER_DATA(buffer), GST_BUFFER_SIZE(buffer));
    gst_buffer_unref(buffer);

    if (pipename == "capture")
        emit v4l2Tools->frameReady(frame);
    else
        emit v4l2Tools->previewFrameReady(frame, pipename);

    v4l2Tools->m_mutex.unlock();
}

void V4L2Tools::setPlaying(bool playing)
{
    this->m_playing = playing;
}

void V4L2Tools::setRecordAudio(bool recordAudio)
{
    this->m_recordAudio = recordAudio;
}

void V4L2Tools::setRecording(bool recording)
{
    this->m_recording = recording;
}

void V4L2Tools::setVideoRecordFormats(QVariantList videoRecordFormats)
{
    this->m_videoRecordFormats = videoRecordFormats;
}

void V4L2Tools::setCurDevName(QString curDevName)
{
    this->m_curDevName = curDevName;
}

void V4L2Tools::setStreams(QVariantList streams)
{
    this->m_streams = streams;
}

void V4L2Tools::resetPlaying()
{
    this->setPlaying(false);
}

void V4L2Tools::resetRecordAudio()
{
    this->setRecordAudio(true);
}

void V4L2Tools::resetRecording()
{
    this->setRecording(false);
}

void V4L2Tools::resetVideoRecordFormats()
{
    this->setVideoRecordFormats(QVariantList());
}

void V4L2Tools::resetCurDevName()
{
    this->setCurDevName("");
}

void V4L2Tools::resetStreams()
{
    this->setStreams(QVariantList());
}

void V4L2Tools::loadConfigs()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(this->m_appEnvironment->configFileName());

    KConfigGroup webcamConfigs = config->group("GeneralConfigs");
    this->enableAudioRecording(webcamConfigs.readEntry("recordAudio", true));

    KConfigGroup effectsConfigs = config->group("Effects");

    QString effcts = effectsConfigs.readEntry("effects", "");

    if (!effcts.isEmpty())
        this->setEffects(effcts.split("&&", QString::SkipEmptyParts));

    KConfigGroup videoFormatsConfigs = config->group("VideoRecordFormats");

    QString videoRecordFormats = videoFormatsConfigs.
                    readEntry("formats",
                              "webm::"
                              "vp8enc quality=10 speed=7 bitrate=1000000000::"
                              "vorbisenc::"
                              "webmmux&&"
                              "ogv, ogg::"
                              "theoraenc quality=63 bitrate=16777215::"
                              "vorbisenc::"
                              "oggmux");

    if (!videoRecordFormats.isEmpty())
        foreach (QString fmt, videoRecordFormats.split("&&", QString::SkipEmptyParts))
        {
            QStringList params = fmt.split("::", QString::SkipEmptyParts);

            this->setVideoRecordFormat(params.at(0),
                                       params.at(1),
                                       params.at(2),
                                       params.at(3));
        }

    KConfigGroup streamsConfig = config->group("CustomStreams");
    QString streams = streamsConfig.readEntry("streams", "");

    if (!streams.isEmpty())
        foreach (QString fmt, streams.split("&&", QString::SkipEmptyParts))
        {
            QStringList params = fmt.split("::", QString::SkipEmptyParts);

            this->setCustomStream(params.at(0), params.at(1));
        }
}

void V4L2Tools::saveConfigs()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(this->m_appEnvironment->configFileName());

    KConfigGroup webcamConfigs = config->group("GeneralConfigs");

    webcamConfigs.writeEntry("recordAudio", this->m_recordAudio);

    KConfigGroup effectsConfigs = config->group("Effects");

    effectsConfigs.writeEntry("effects", this->m_effects.join("&&"));

    KConfigGroup videoFormatsConfigs = config->group("VideoRecordFormats");

    QStringList videoRecordFormats;

    foreach (QVariant format, this->m_videoRecordFormats)
        videoRecordFormats << QString("%1::%2::%3::%4").arg(format.toList().at(0).toString())
                                                       .arg(format.toList().at(1).toString())
                                                       .arg(format.toList().at(2).toString())
                                                       .arg(format.toList().at(3).toString());

    videoFormatsConfigs.writeEntry("formats",
                                   videoRecordFormats.join("&&"));

    KConfigGroup streamsConfigs = config->group("CustomStreams");

    QStringList streams;

    foreach (QVariant stream, this->m_streams)
        streams << QString("%1::%2").arg(stream.toList().at(0).toString())
                                    .arg(stream.toList().at(1).toString());

    streamsConfigs.writeEntry("streams", streams.join("&&"));

    config->sync();
}

void V4L2Tools::setEffects(QStringList effects)
{
    if (this->m_effects == effects)
        return;

    this->m_effects = effects;

    GstState state;
    GstState pending;

    gst_element_get_state(this->m_mainPipeline, &state, &pending, GST_CLOCK_TIME_NONE);

    if (pending == GST_STATE_PLAYING)
        gst_element_set_state(this->m_mainPipeline, GST_STATE_PAUSED);

    GstElement *captureTee = gst_bin_get_by_name(GST_BIN(this->m_mainBin), "CaptureTee");
    GstElement *effectsTee = gst_bin_get_by_name(GST_BIN(this->m_mainBin), "EffectsTee");

    if (this->m_effectsBin)
    {
        GstElement *effectsBinIn = gst_bin_get_by_name(GST_BIN(this->m_effectsBin), "EffectsBinIn");
        GstElement *effectsBinOut = gst_bin_get_by_name(GST_BIN(this->m_effectsBin), "EffectsBinOut");

        gst_element_unlink(captureTee, effectsBinIn);
        gst_element_unlink(effectsBinOut, effectsTee);
        gst_bin_remove(GST_BIN(this->m_mainBin), this->m_effectsBin);
        gst_element_link(captureTee, effectsTee);

        gst_object_unref(GST_OBJECT(this->m_effectsBin));
        gst_object_unref(GST_OBJECT(effectsBinIn));
        gst_object_unref(GST_OBJECT(effectsBinOut));
    }

    if (!this->m_effects.isEmpty())
    {
        QString pipeline = "queue name=EffectsBinIn";

        foreach (QString effect, this->m_effects)
            pipeline += QString(" ! ffmpegcolorspace ! %1").arg(effect);

        pipeline += " ! identity name=EffectsBinOut";

        GError *error = NULL;

        this->m_effectsBin = gst_parse_bin_from_description(pipeline.toUtf8().constData(),
                                                            FALSE,
                                                            &error);

        g_object_set(GST_OBJECT(this->m_effectsBin), "name", "effectsBin", NULL);

        GstElement *effectsBinIn = gst_bin_get_by_name(GST_BIN(this->m_effectsBin), "EffectsBinIn");
        GstElement *effectsBinOut = gst_bin_get_by_name(GST_BIN(this->m_effectsBin), "EffectsBinOut");

        gst_element_unlink(captureTee, effectsTee);
        gst_bin_add(GST_BIN(this->m_mainBin), this->m_effectsBin);
        gst_element_link(captureTee, effectsBinIn);
        gst_element_link(effectsBinOut, effectsTee);

        gst_object_unref(GST_OBJECT(effectsBinIn));
        gst_object_unref(GST_OBJECT(effectsBinOut));
    }

    gst_object_unref(GST_OBJECT(captureTee));
    gst_object_unref(GST_OBJECT(effectsTee));

    if (pending == GST_STATE_PLAYING)
        gst_element_set_state(this->m_mainPipeline, GST_STATE_PLAYING);
}

void V4L2Tools::startEffectsPreview()
{
    GstState state;
    GstState pending;

    gst_element_get_state(this->m_mainPipeline, &state, &pending, GST_CLOCK_TIME_NONE);

    if (pending == GST_STATE_PLAYING)
        gst_element_set_state(this->m_mainPipeline, GST_STATE_PAUSED);

    if (!this->m_effectsPreviewBin)
    {
        QString pipeline = QString("queue name=EffectsPreview ! valve name=EffectsPreviewValve drop=true ! ffmpegcolorspace ! "
                                   "videoscale ! video/x-raw-rgb,width=%1,height=%2 ! tee name=preview").arg(128)
                                                                                                        .arg(96);

        foreach (QString effect, this->availableEffects().keys())
        {
            QString previewHash = this->hashFromName(effect);

            pipeline += QString(" preview. ! queue ! ffmpegcolorspace ! %1 ! "
                                "ffmpegcolorspace ! ffenc_bmp ! "
                                "appsink name=%2 emit-signals=true").arg(effect).arg(previewHash);
        }

        GError *error = NULL;

        this->m_effectsPreviewBin = gst_parse_bin_from_description(pipeline.toUtf8().constData(),
                                                                   FALSE,
                                                                   &error);

        g_object_set(GST_OBJECT(this->m_effectsPreviewBin), "name", "effectsPreviewBin", NULL);

        gst_bin_add(GST_BIN(this->m_mainBin), this->m_effectsPreviewBin);
        GstElement *queueEffectsPreview = gst_bin_get_by_name(GST_BIN(this->m_effectsPreviewBin), "EffectsPreview");
        GstElement *captureTee = gst_bin_get_by_name(GST_BIN(this->m_mainBin), "CaptureTee");
        gst_element_link(captureTee, queueEffectsPreview);

        foreach (QString effect, this->availableEffects().keys())
        {
            QString previewHash = this->hashFromName(effect);
            GstElement *preview = gst_bin_get_by_name(GST_BIN(this->m_mainBin), previewHash.toUtf8().constData());

            g_signal_connect(preview, "new-buffer",  G_CALLBACK(V4L2Tools::readFrame), this);

            gst_object_unref(GST_OBJECT(preview));
        }

        gst_object_unref(GST_OBJECT(queueEffectsPreview));
        gst_object_unref(GST_OBJECT(captureTee));
    }

    GstElement *effectsPreviewValve = gst_bin_get_by_name(GST_BIN(this->m_effectsPreviewBin), "EffectsPreviewValve");
    g_object_set(GST_OBJECT(effectsPreviewValve), "drop", FALSE, NULL);
    gst_object_unref(GST_OBJECT(effectsPreviewValve));

    if (pending == GST_STATE_PLAYING)
        gst_element_set_state(this->m_mainPipeline, GST_STATE_PLAYING);
}

void V4L2Tools::stopEffectsPreview()
{
    if (m_effectsPreviewBin)
    {
        GstState state;
        GstState pending;

        gst_element_get_state(this->m_mainPipeline, &state, &pending, GST_CLOCK_TIME_NONE);

        if (pending == GST_STATE_PLAYING)
            gst_element_set_state(this->m_mainPipeline, GST_STATE_PAUSED);

        GstElement *effectsPreviewValve = gst_bin_get_by_name(GST_BIN(this->m_effectsPreviewBin), "EffectsPreviewValve");
        g_object_set(GST_OBJECT(effectsPreviewValve), "drop", FALSE, NULL);
        gst_object_unref(GST_OBJECT(effectsPreviewValve));

        if (pending == GST_STATE_PLAYING)
            gst_element_set_state(this->m_mainPipeline, GST_STATE_PLAYING);
    }
}

void V4L2Tools::startDevice(QString dev_name, QVariantList forcedFormat)
{
    this->stopCurrentDevice();
    StreamType deviceType = this->deviceType(dev_name);
    GError *error = NULL;

    if (deviceType == StreamTypeWebcam)
    {
        QVariantList fmt;

        if (forcedFormat.isEmpty())
        {
            fmt = this->currentVideoFormat(dev_name);

            if (fmt.isEmpty())
                fmt = this->videoFormats(dev_name).at(0).toList();
        }
        else
            fmt = forcedFormat;

        QString description = QString("v4l2src device=%1 ! "
                                      "capsfilter name=capture "
                                      "caps=video/x-raw-yuv,width=%2,height=%3").arg(dev_name)
                                                                                .arg(fmt.at(0).toInt())
                                                                                .arg(fmt.at(1).toInt());

        this->m_captureDevice = gst_parse_bin_from_description(description.toUtf8().constData(),
                                                               FALSE,
                                                               &error);

        g_object_set(GST_OBJECT(this->m_captureDevice), "name", "captureDevice", NULL);
    }
    else if (deviceType == StreamTypeURI)
    {
        this->m_captureDevice = gst_parse_bin_from_description("uridecodebin name=capture",
                                                               FALSE,
                                                               &error);

        g_object_set(GST_OBJECT(this->m_captureDevice), "name", "captureDevice", NULL);

        GstElement *capture = gst_bin_get_by_name(GST_BIN(this->m_captureDevice), "capture");
        g_object_set(GST_OBJECT(capture), "uri", dev_name.toUtf8().constData(), NULL);
        gst_object_unref(GST_OBJECT(capture));
    }
    else if (deviceType == StreamTypeDesktop)
    {
        this->m_captureDevice = gst_parse_bin_from_description("ximagesrc name=capture show-pointer=true",
                                                               FALSE,
                                                               &error);

        g_object_set(GST_OBJECT(this->m_captureDevice), "name", "captureDevice", NULL);
    }
    else
        return;

    gst_bin_add(GST_BIN(this->m_mainBin), this->m_captureDevice);
    GstElement *capture = gst_bin_get_by_name(GST_BIN(this->m_captureDevice), "capture");
    GstElement *captureTee = gst_bin_get_by_name(GST_BIN(this->m_mainBin), "CaptureTee");
    gst_element_link(capture, captureTee);
    gst_object_unref(GST_OBJECT(capture));
    gst_object_unref(GST_OBJECT(captureTee));

    gst_element_set_state(this->m_mainPipeline, GST_STATE_PLAYING);
    this->m_curDevName = dev_name;
    this->m_playing = true;

    emit this->playingStateChanged(true);

    //self.gstError.emit()
}

void V4L2Tools::stopCurrentDevice()
{
    if (!this->m_playing)
        return;

    gst_element_set_state(this->m_mainPipeline, GST_STATE_NULL);

    GstElement *capture = gst_bin_get_by_name(GST_BIN(this->m_captureDevice), "capture");
    GstElement *captureTee = gst_bin_get_by_name(GST_BIN(this->m_mainBin), "CaptureTee");
    gst_element_unlink(capture, captureTee);
    gst_bin_remove(GST_BIN(this->m_mainBin), this->m_captureDevice);
    gst_object_unref(GST_OBJECT(capture));
    gst_object_unref(GST_OBJECT(captureTee));

    this->m_curDevName = "";
    this->m_playing = false;
    emit this->playingStateChanged(false);
}

void V4L2Tools::startVideoRecord(QString fileName)
{
    Q_UNUSED(fileName)
    /*
    this->stopVideoRecord();

    if (!this->m_playing)
        return;

    // suffix, videoEncoder, audioEncoder, muxer
    QStringList format = this->bestVideoRecordFormat(fileName);

    if (format.at(0).isEmpty())
        return;

    QString pipeline = QString("appsrc name={capture} emit-signals=true do-timestamp=true ! ffdec_bmp ! "
                               "ffmpegcolorspace ! %1 ! queue ! muxer. ").arg(videoEncoder);

    if (this->m_recordAudio)
        // autoaudiosrc
        pipeline += QString("alsasrc device=plughw:0,0 ! queue ! audioconvert ! "
                            "queue ! %1 ! queue ! muxer. ").arg(audioEncoder);

    pipeline += QString("%1 name=muxer ! filesink location=\"%2\"").arg(muxer)
                                                                   .arg(fileName);

    this->pipeRecordVideo->setPipeline(pipeline);
    self.pipeRecordVideo.start()
    emit this->recordingStateChanged(true);
    this->m_recording = True;*/
}

void V4L2Tools::stopVideoRecord()
{
    if (this->m_recording)
    {
        //dev_name = this->curDevName
        //this->pipeRecordVideo->stop();
        this->m_recording = false;
        emit this->recordingStateChanged(false);
    }
}

void V4L2Tools::clearVideoRecordFormats()
{
    this->m_videoRecordFormats.clear();
}

void V4L2Tools::clearCustomStreams()
{
    this->m_streams.clear();
    emit this->devicesModified();
}

void V4L2Tools::setCustomStream(QString dev_name, QString description)
{
    this->m_streams << QVariant(QVariantList() << dev_name
                                               << description
                                               << StreamTypeURI);

    emit this->devicesModified();
}

void V4L2Tools::enableAudioRecording(bool enable)
{
    this->m_recordAudio = enable;
}

void V4L2Tools::setVideoRecordFormat(QString suffix, QString videoEncoder,
                                     QString audioEncoder, QString muxer)
{
    this->m_videoRecordFormats << QVariant(QVariantList() << suffix
                                                          << videoEncoder
                                                          << audioEncoder
                                                          << muxer);
}

void V4L2Tools::aboutToQuit()
{
    this->stopCurrentDevice();
    this->saveConfigs();
}

void V4L2Tools::reset(QString dev_name)
{
    QVariantList videoFormats = this->videoFormats(dev_name);
    this->setVideoFormat(dev_name, videoFormats.at(0).toList());

    QVariantList controls = this->listControls(dev_name);
    QMap<QString, uint> ctrls;

    foreach (QVariant control, controls)
        ctrls[control.toList().at(0).toString()] = control.toList().at(5).toUInt();

    this->setControls(dev_name, ctrls);
}

void V4L2Tools::onDirectoryChanged(const QString &path)
{
    Q_UNUSED(path)

    emit this->devicesModified();
}
