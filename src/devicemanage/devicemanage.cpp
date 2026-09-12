#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include "devicemanage.h"
#include "device.h"
#include "demuxer.h"

namespace qsc {

#define DM_MAX_DEVICES_NUM 1000

IDeviceManage& IDeviceManage::getInstance() {
    static DeviceManage dm;
    return dm;
}

DeviceManage::DeviceManage() {
    Demuxer::init();
}

DeviceManage::~DeviceManage() {
    Demuxer::deInit();
}

QPointer<IDevice> DeviceManage::getDevice(const QString &serial)
{
    if (!m_devices.contains(serial)) {
        return QPointer<IDevice>();
    }
    return m_devices[serial];
}

bool DeviceManage::connectDevice(qsc::DeviceParams params)
{
    if (params.serial.trimmed().isEmpty()) {
        return false;
    }
    if (m_devices.contains(params.serial)) {
        return false;
    }
    if (DM_MAX_DEVICES_NUM < m_devices.size()) {
        qInfo("over the maximum number of connections");
        return false;
    }
    // Every reverse tunnel binds a host TCP listener.  Multiple devices start
    // concurrently in IMM, so sharing 27183 makes all but the first server
    // fail with "Could not listen on port 27183".
    quint16 port = 0;
    if (params.useReverse) {
        port = getFreePort();
        if (0 == port) {
            qInfo("no port available, automatically switch to forward");
            params.useReverse = false;
        } else {
            params.localPort = port;
            qInfo("free port %d", port);
        }
    }
    IDevice *device = new Device(params);
    connect(device, &Device::deviceConnected, this, &DeviceManage::onDeviceConnected);
    connect(device, &Device::deviceDisconnected, this, &DeviceManage::onDeviceDisconnected);
    if (!device->connectDevice()) {
        delete device;
        return false;
    }
    m_devices[params.serial] = device;
    return true;
}

// Explicit disconnects must drop the map entry themselves. Device's
// destructor only emits deviceDisconnected (which is what used to remove the
// entry) when the server had fully started -- so disconnecting a device that
// was still *connecting* (queue timeout, unplug during start-up, 停止全部
// mid-connect) left a dangling key behind, and connectDevice() then refused
// that serial forever with "already connected". That was the "phone dropped
// once, can never reconnect until restart" bug.
bool DeviceManage::disconnectDevice(const QString &serial)
{
    if (serial.isEmpty() || !m_devices.contains(serial)) {
        return false;
    }
    QPointer<IDevice> device = m_devices.take(serial);
    if (!device) {
        return false;
    }
    delete device;
    return true;
}

void DeviceManage::disconnectAllDevice()
{
    const QMap<QString, QPointer<IDevice>> devices = m_devices;
    m_devices.clear();
    for (auto it = devices.cbegin(); it != devices.cend(); ++it) {
        if (it.value()) {
            delete it.value();
        }
    }
}

void DeviceManage::onDeviceConnected(bool success, const QString &serial, const QString &deviceName, const QSize &size)
{
    emit deviceConnected(success, serial, deviceName, size);
    if (!success) {
        removeDevice(serial);
    }
}

void DeviceManage::onDeviceDisconnected(QString serial)
{
    emit deviceDisconnected(serial);
    removeDevice(serial);
}

quint16 DeviceManage::getFreePort()
{
    quint16 port = m_localPortStart;
    while (port < m_localPortStart + DM_MAX_DEVICES_NUM) {
        bool used = false;
        QMapIterator<QString, QPointer<IDevice>> i(m_devices);
        while (i.hasNext()) {
            i.next();
            auto device = i.value();
            if (device && device->isReversePort(port)) {
                used = true;
                break;
            }
        }
        if (!used) {
            return port;
        }
        port++;
    }
    return 0;
}

void DeviceManage::removeDevice(const QString &serial)
{
    if (serial.isEmpty() || !m_devices.contains(serial)) {
        return;
    }
    QPointer<IDevice> device = m_devices.take(serial);
    if (device) {
        device->deleteLater();
    }
}

}
