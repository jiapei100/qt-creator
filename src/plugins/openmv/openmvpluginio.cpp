#include "openmvpluginio.h"

#define MTU_DEFAULT_SIZE (1024 * 1024 * 1024)

enum
{
    USBDBG_FW_VERSION_CPL,
    USBDBG_FRAME_SIZE_CPL,
    USBDBG_FRAME_DUMP_CPL,
    USBDBG_ARCH_STR_CPL,
    USBDBG_LEARN_MTU_CPL,
    USBDBG_SCRIPT_EXEC_CPL_0,
    USBDBG_SCRIPT_EXEC_CPL_1,
    USBDBG_SCRIPT_STOP_CPL,
    USBDBG_SCRIPT_RUNNING_CPL,
    USBDBG_SCRIPT_SAVE_CPL,
    USBDBG_TEMPLATE_SAVE_CPL_0,
    USBDBG_TEMPLATE_SAVE_CPL_1,
    USBDBG_DESCRIPTOR_SAVE_CPL_0,
    USBDBG_DESCRIPTOR_SAVE_CPL_1,
    USBDBG_ATTR_READ_CPL,
    USBDBG_ATTR_WRITE_CPL,
    USBDBG_SYS_RESET_CPL,
    USBDBG_FB_ENABLE_CPL,
    USBDBG_JPEG_ENABLE_CPL,
    USBDBG_TX_BUF_LEN_CPL,
    USBDBG_TX_BUF_CPL,
    BOOTLDR_START_CPL,
    BOOTLDR_RESET_CPL,
    BOOTLDR_ERASE_CPL,
    BOOTLDR_WRITE_CPL,
    BOOTLDR_QUERY_CPL,
    CLOSE_CPL
};

static QByteArray byteSwap(QByteArray buffer, bool ok)
{
    if(ok)
    {
        for(int i = 0, j = (buffer.size() / 2) * 2; i < j; i += 2)
        {
            char temp = buffer.data()[i];
            buffer.data()[i] = buffer.data()[i+1];
            buffer.data()[i+1] = temp;
        }
    }

    return buffer;
}

int getImageSize(int w, int h, int bpp)
{
    return IS_JPG(bpp) ? bpp : ((IS_RGB(bpp) || IS_GS(bpp)) ? (w * h * bpp) : (IS_BINARY(bpp) ? (((w + 31) / 32) * h) : int()));
}

QPixmap getImageFromData(QByteArray data, int w, int h, int bpp)
{
    QPixmap pixmap = getImageSize(w, h, bpp) ? (QPixmap::fromImage(IS_JPG(bpp)
        ? QImage::fromData(data, "JPG")
        : QImage(reinterpret_cast<const uchar *>(byteSwap(data,
            IS_RGB(bpp)).constData()), w, h, IS_BINARY(bpp) ? ((w + 31) / 32) : (w * bpp),
            IS_RGB(bpp) ? QImage::Format_RGB16 : (IS_GS(bpp) ? QImage::Format_Grayscale8 : (IS_BINARY(bpp) ? QImage::Format_MonoLSB : QImage::Format_Invalid)))))
    : QPixmap();

    if(pixmap.isNull() && IS_JPG(bpp))
    {
        data = data.mid(1, data.size() - 2);

        int size = data.size();
        QByteArray temp;

        for(int i = 0, j = (size / 4) * 4; i < j; i += 4)
        {
            int x = 0;
            x |= (data.at(i + 0) & 0x3F) << 0;
            x |= (data.at(i + 1) & 0x3F) << 6;
            x |= (data.at(i + 2) & 0x3F) << 12;
            x |= (data.at(i + 3) & 0x3F) << 18;
            temp.append((x >> 0) & 0xFF);
            temp.append((x >> 8) & 0xFF);
            temp.append((x >> 16) & 0xFF);
        }

        if((size % 4) == 3) // 2 bytes -> 16-bits -> 24-bits sent
        {
            int x = 0;
            x |= (data.at(size - 3) & 0x3F) << 0;
            x |= (data.at(size - 2) & 0x3F) << 6;
            x |= (data.at(size - 1) & 0x0F) << 12;
            temp.append((x >> 0) & 0xFF);
            temp.append((x >> 8) & 0xFF);
        }

        if((size % 4) == 2) // 1 byte -> 8-bits -> 16-bits sent
        {
            int x = 0;
            x |= (data.at(size - 2) & 0x3F) << 0;
            x |= (data.at(size - 1) & 0x03) << 6;
            temp.append((x >> 0) & 0xFF);
        }

        pixmap = QPixmap::fromImage(QImage::fromData(temp, "JPG"));
    }

    return pixmap;
}

OpenMVPluginIO::OpenMVPluginIO(OpenMVPluginSerialPort *port, QObject *parent) : QObject(parent)
{
    m_port = port;

    connect(m_port, &OpenMVPluginSerialPort::commandResult,
            this, &OpenMVPluginIO::commandResult);

    m_postedQueue = QQueue<OpenMVPluginSerialPortCommand>();
    m_completionQueue = QQueue<int>();
    m_frameSizeW = int();
    m_frameSizeH = int();
    m_frameSizeBPP = int();
    m_mtu = MTU_DEFAULT_SIZE;
    m_pixelBuffer = QByteArray();
    m_lineBuffer = QByteArray();
    m_timeout = bool();
}

void OpenMVPluginIO::command()
{
    if((!m_postedQueue.isEmpty())
    && (!m_completionQueue.isEmpty())
    && (m_postedQueue.size() == m_completionQueue.size()))
    {
        m_port->command(m_postedQueue.dequeue());
    }
}

void OpenMVPluginIO::commandResult(const OpenMVPluginSerialPortCommandResult &commandResult)
{
    if(Q_LIKELY(!m_completionQueue.isEmpty()))
    {
        if(commandResult.m_ok)
        {
            QByteArray data = commandResult.m_data;

            switch(m_completionQueue.head())
            {
                case USBDBG_FW_VERSION_CPL:
                {
                    // The optimizer will mess up the order if executed in emit.
                    int major = deserializeLong(data);
                    int minor = deserializeLong(data);
                    int patch = deserializeLong(data);
                    emit firmwareVersion(major, minor, patch);
                    break;
                }
                case USBDBG_FRAME_SIZE_CPL:
                {
                    int w = deserializeLong(data);
                    int h = deserializeLong(data);
                    int bpp = deserializeLong(data);

                    if((0 < w) && (w < 32768) && (0 < h) && (h < 32768) && (0 <= bpp) && (bpp <= (1024 * 1024 * 1024)))
                    {
                        int size = getImageSize(w, h, bpp);

                        if(size)
                        {
                            for(int i = 0, j = (size + m_mtu - 1) / m_mtu; i < j; i++)
                            {
                                int new_size = qMin(size - ((j - 1 - i) * m_mtu), m_mtu);
                                QByteArray buffer;
                                serializeByte(buffer, __USBDBG_CMD);
                                serializeByte(buffer, __USBDBG_FRAME_DUMP);
                                serializeLong(buffer, new_size);
                                m_postedQueue.push_front(OpenMVPluginSerialPortCommand(buffer, new_size, FRAME_DUMP_START_DELAY, FRAME_DUMP_END_DELAY));
                                m_completionQueue.insert(1, USBDBG_FRAME_DUMP_CPL);
                            }

                            m_frameSizeW = w;
                            m_frameSizeH = h;
                            m_frameSizeBPP = bpp;
                        }
                    }

                    break;
                }
                case USBDBG_FRAME_DUMP_CPL:
                {
                    m_pixelBuffer.append(data);

                    if(m_pixelBuffer.size() == getImageSize(m_frameSizeW, m_frameSizeH, m_frameSizeBPP))
                    {
                        QPixmap pixmap = getImageFromData(m_pixelBuffer, m_frameSizeW, m_frameSizeH, m_frameSizeBPP);

                        if(!pixmap.isNull())
                        {
                            emit frameBufferData(pixmap);
                        }

                        m_frameSizeW = int();
                        m_frameSizeH = int();
                        m_frameSizeBPP = int();
                        m_pixelBuffer.clear();
                    }

                    break;
                }
                case USBDBG_ARCH_STR_CPL:
                {
                    emit archString(QString::fromUtf8(data));
                    break;
                }
                case USBDBG_LEARN_MTU_CPL:
                {
                    m_mtu = deserializeLong(data);
                    emit learnedMTU(true);
                    break;
                }
                case USBDBG_SCRIPT_EXEC_CPL_0:
                {
                    break;
                }
                case USBDBG_SCRIPT_EXEC_CPL_1:
                {
                    emit scriptExecDone();
                    break;
                }
                case USBDBG_SCRIPT_STOP_CPL:
                {
                    emit scriptStopDone();
                    break;
                }
                case USBDBG_SCRIPT_RUNNING_CPL:
                {
                    emit scriptRunning(deserializeLong(data));
                    break;
                }
                case USBDBG_TEMPLATE_SAVE_CPL_0:
                {
                    break;
                }
                case USBDBG_TEMPLATE_SAVE_CPL_1:
                {
                    emit templateSaveDone();
                    break;
                }
                case USBDBG_DESCRIPTOR_SAVE_CPL_0:
                {
                    break;
                }
                case USBDBG_DESCRIPTOR_SAVE_CPL_1:
                {
                    emit descriptorSaveDone();
                    break;
                }
                case USBDBG_ATTR_READ_CPL:
                {
                    emit attribute(deserializeByte(data));
                    break;
                }
                case USBDBG_ATTR_WRITE_CPL:
                {
                    emit setAttrributeDone();
                    break;
                }
                case USBDBG_SYS_RESET_CPL:
                {
                    emit sysResetDone();
                    break;
                }
                case USBDBG_FB_ENABLE_CPL:
                {
                    emit fbEnableDone();
                    break;
                }
                case USBDBG_JPEG_ENABLE_CPL:
                {
                    emit jpegEnableDone();
                    break;
                }
                case USBDBG_TX_BUF_LEN_CPL:
                {
                    int len = deserializeLong(data);

                    if(len)
                    {
                        for(int i = 0, j = (len + m_mtu - 1) / m_mtu; i < j; i++)
                        {
                            int new_len = qMin(len - ((j - 1 - i) * m_mtu), m_mtu);
                            QByteArray buffer;
                            serializeByte(buffer, __USBDBG_CMD);
                            serializeByte(buffer, __USBDBG_TX_BUF);
                            serializeLong(buffer, new_len);
                            m_postedQueue.push_front(OpenMVPluginSerialPortCommand(buffer, new_len, TX_BUF_START_DELAY, TX_BUF_END_DELAY));
                            m_completionQueue.insert(1, USBDBG_TX_BUF_CPL);
                        }
                    }
                    else if(m_lineBuffer.size())
                    {
                        emit printData(pasrsePrintData(m_lineBuffer));
                        m_lineBuffer.clear();
                    }

                    break;
                }
                case USBDBG_TX_BUF_CPL:
                {
                    m_lineBuffer.append(data);
                    QByteArrayList list = m_lineBuffer.split('\n');
                    m_lineBuffer = list.takeLast();

                    QByteArray out;

                    for(int i = 0, j = list.size(); i < j; i++)
                    {
                        out.append(pasrsePrintData(list.at(i) + '\n'));
                    }

                    if(!out.isEmpty())
                    {
                        emit printData(out);
                    }

                    break;
                }
                case BOOTLDR_START_CPL:
                {
                    int result = deserializeLong(data);
                    emit gotBootloaderStart((result == OLD_BOOTLDR) || (result == NEW_BOOTLDR), result);
                    break;
                }
                case BOOTLDR_RESET_CPL:
                {
                    emit bootloaderResetDone(true);
                    break;
                }
                case BOOTLDR_ERASE_CPL:
                {
                    emit flashEraseDone(true);
                    break;
                }
                case BOOTLDR_WRITE_CPL:
                {
                    emit flashWriteDone(true);
                    break;
                }
                case BOOTLDR_QUERY_CPL:
                {
                    // The optimizer will mess up the order if executed in emit.
                    int all_start = deserializeLong(data);
                    int start = deserializeLong(data);
                    int last = deserializeLong(data);
                    emit bootloaderQueryDone(all_start, start, last);
                    break;
                }
                case CLOSE_CPL:
                {
                    if(m_lineBuffer.size())
                    {
                        emit printData(pasrsePrintData(m_lineBuffer));
                        m_lineBuffer.clear();
                    }

                    emit closeResponse();
                    break;
                }
            }

            m_completionQueue.dequeue();

            command();
        }
        else
        {
            forever
            {
                switch(m_completionQueue.head())
                {
                    case USBDBG_FW_VERSION_CPL:
                    {
                        emit firmwareVersion(int(), int(), int());
                        break;
                    }
                    case USBDBG_FRAME_SIZE_CPL:
                    {
                        break;
                    }
                    case USBDBG_FRAME_DUMP_CPL:
                    {
                        m_frameSizeW = int();
                        m_frameSizeH = int();
                        m_frameSizeBPP = int();
                        m_pixelBuffer.clear();
                        break;
                    }
                    case USBDBG_ARCH_STR_CPL:
                    {
                        emit archString(QString());
                        break;
                    }
                    case USBDBG_LEARN_MTU_CPL:
                    {
                        m_mtu = MTU_DEFAULT_SIZE;
                        emit learnedMTU(false);
                        break;
                    }
                    case USBDBG_SCRIPT_EXEC_CPL_0:
                    {
                        break;
                    }
                    case USBDBG_SCRIPT_EXEC_CPL_1:
                    {
                        emit scriptExecDone();
                        break;
                    }
                    case USBDBG_SCRIPT_STOP_CPL:
                    {
                        emit scriptStopDone();
                        break;
                    }
                    case USBDBG_SCRIPT_RUNNING_CPL:
                    {
                        emit scriptRunning(bool());
                        break;
                    }
                    case USBDBG_TEMPLATE_SAVE_CPL_0:
                    {
                        break;
                    }
                    case USBDBG_TEMPLATE_SAVE_CPL_1:
                    {
                        emit templateSaveDone();
                        break;
                    }
                    case USBDBG_DESCRIPTOR_SAVE_CPL_0:
                    {
                        break;
                    }
                    case USBDBG_DESCRIPTOR_SAVE_CPL_1:
                    {
                        emit descriptorSaveDone();
                        break;
                    }
                    case USBDBG_ATTR_READ_CPL:
                    {
                        emit attribute(int());
                        break;
                    }
                    case USBDBG_ATTR_WRITE_CPL:
                    {
                        emit setAttrributeDone();
                        break;
                    }
                    case USBDBG_SYS_RESET_CPL:
                    {
                        emit sysResetDone();
                        break;
                    }
                    case USBDBG_FB_ENABLE_CPL:
                    {
                        emit fbEnableDone();
                        break;
                    }
                    case USBDBG_JPEG_ENABLE_CPL:
                    {
                        emit jpegEnableDone();
                        break;
                    }
                    case USBDBG_TX_BUF_LEN_CPL:
                    {
                        if(m_lineBuffer.size())
                        {
                            emit printData(pasrsePrintData(m_lineBuffer));
                            m_lineBuffer.clear();
                        }

                        break;
                    }
                    case USBDBG_TX_BUF_CPL:
                    {
                        if(m_lineBuffer.size())
                        {
                            emit printData(pasrsePrintData(m_lineBuffer));
                            m_lineBuffer.clear();
                        }

                        break;
                    }
                    case BOOTLDR_START_CPL:
                    {
                        emit gotBootloaderStart(false, int());
                        break;
                    }
                    case BOOTLDR_RESET_CPL:
                    {
                        emit bootloaderResetDone(false);
                        break;
                    }
                    case BOOTLDR_ERASE_CPL:
                    {
                        emit flashEraseDone(false);
                        break;
                    }
                    case BOOTLDR_WRITE_CPL:
                    {
                        emit flashWriteDone(false);
                        break;
                    }
                    case BOOTLDR_QUERY_CPL:
                    {
                        emit bootloaderQueryDone(int(), int(), int());
                        break;
                    }
                    case CLOSE_CPL:
                    {
                        if(m_lineBuffer.size())
                        {
                            emit printData(pasrsePrintData(m_lineBuffer));
                            m_lineBuffer.clear();
                        }

                        emit closeResponse();
                        break;
                    }
                }

                m_completionQueue.dequeue();

                if((!m_postedQueue.isEmpty())
                && (!m_completionQueue.isEmpty())
                && (m_postedQueue.size() == m_completionQueue.size()))
                {
                    m_postedQueue.dequeue();
                }
                else
                {
                    break;
                }
            }

            m_timeout = true;
        }
    }
}

bool OpenMVPluginIO::getTimeout()
{
    bool timeout = m_timeout;
    m_timeout = false;
    return timeout;
}

bool OpenMVPluginIO::frameSizeDumpQueued() const
{
    return m_completionQueue.contains(USBDBG_FRAME_SIZE_CPL) ||
           m_completionQueue.contains(USBDBG_FRAME_DUMP_CPL);
}

bool OpenMVPluginIO::getScriptRunningQueued() const
{
    return m_completionQueue.contains(USBDBG_SCRIPT_RUNNING_CPL);
}

bool OpenMVPluginIO::getAttributeQueued() const
{
    return m_completionQueue.contains(USBDBG_ATTR_READ_CPL);
}

bool OpenMVPluginIO::getTxBufferQueued() const
{
    return m_completionQueue.contains(USBDBG_TX_BUF_LEN_CPL) ||
           m_completionQueue.contains(USBDBG_TX_BUF_CPL);
}

void OpenMVPluginIO::getFirmwareVersion()
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_FW_VERSION);
    serializeLong(buffer, FW_VERSION_RESPONSE_LEN);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, FW_VERSION_RESPONSE_LEN, FW_VERSION_START_DELAY, FW_VERSION_END_DELAY));
    m_completionQueue.enqueue(USBDBG_FW_VERSION_CPL);
    command();
}

void OpenMVPluginIO::frameSizeDump()
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_FRAME_SIZE);
    serializeLong(buffer, FRAME_SIZE_RESPONSE_LEN);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, FRAME_SIZE_RESPONSE_LEN, FRAME_SIZE_START_DELAY, FRAME_SIZE_END_DELAY));
    m_completionQueue.enqueue(USBDBG_FRAME_SIZE_CPL);
    command();
}

void OpenMVPluginIO::getArchString()
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_ARCH_STR);
    serializeLong(buffer, ARCH_STR_RESPONSE_LEN);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, ARCH_STR_RESPONSE_LEN, ARCH_STR_START_DELAY, ARCH_STR_END_DELAY));
    m_completionQueue.enqueue(USBDBG_ARCH_STR_CPL);
    command();
}

void OpenMVPluginIO::learnMTU()
{
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(QByteArray(), sizeof(int), LEARN_MTU_START_DELAY, LEARN_MTU_END_DELAY));
    m_completionQueue.enqueue(USBDBG_LEARN_MTU_CPL);
    command();
}

void OpenMVPluginIO::scriptExec(const QByteArray &data)
{
    QByteArray buffer, script = (data.size() % TABOO_PACKET_SIZE) ? data : (data + '\n');
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_SCRIPT_EXEC);
    serializeLong(buffer, script.size());
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, int(), SCRIPT_EXEC_START_DELAY, SCRIPT_EXEC_END_DELAY));
    m_completionQueue.enqueue(USBDBG_SCRIPT_EXEC_CPL_0);
    command();
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(script, int(), SCRIPT_EXEC_2_START_DELAY, SCRIPT_EXEC_2_END_DELAY));
    m_completionQueue.enqueue(USBDBG_SCRIPT_EXEC_CPL_1);
    command();
}

void OpenMVPluginIO::scriptStop()
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_SCRIPT_STOP);
    serializeLong(buffer, int());
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, int(), SCRIPT_STOP_START_DELAY, SCRIPT_STOP_END_DELAY));
    m_completionQueue.enqueue(USBDBG_SCRIPT_STOP_CPL);
    command();
}

void OpenMVPluginIO::getScriptRunning()
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_SCRIPT_RUNNING);
    serializeLong(buffer, SCRIPT_RUNNING_RESPONSE_LEN);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, SCRIPT_RUNNING_RESPONSE_LEN, SCRIPT_RUNNING_START_DELAY, SCRIPT_RUNNING_END_DELAY));
    m_completionQueue.enqueue(USBDBG_SCRIPT_RUNNING_CPL);
    command();
}

void OpenMVPluginIO::templateSave(int x, int y, int w, int h, const QByteArray &path)
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_TEMPLATE_SAVE);
    serializeLong(buffer, 2 + 2 + 2 + 2 + path.size());
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, int(), TEMPLATE_SAVE_START_DELAY, TEMPLATE_SAVE_END_DELAY));
    m_completionQueue.enqueue(USBDBG_TEMPLATE_SAVE_CPL_0);
    command();
    buffer.clear();
    serializeWord(buffer, x);
    serializeWord(buffer, y);
    serializeWord(buffer, w);
    serializeWord(buffer, h);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer + path, int(), TEMPLATE_SAVE_2_START_DELAY, TEMPLATE_SAVE_2_END_DELAY));
    m_completionQueue.enqueue(USBDBG_TEMPLATE_SAVE_CPL_1);
    command();
}

void OpenMVPluginIO::descriptorSave(int x, int y, int w, int h, const QByteArray &path)
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_DESCRIPTOR_SAVE);
    serializeLong(buffer, 2 + 2 + 2 + 2 + path.size());
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, int(), DESCRIPTOR_SAVE_START_DELAY, DESCRIPTOR_SAVE_END_DELAY));
    m_completionQueue.enqueue(USBDBG_DESCRIPTOR_SAVE_CPL_0);
    command();
    buffer.clear();
    serializeWord(buffer, x);
    serializeWord(buffer, y);
    serializeWord(buffer, w);
    serializeWord(buffer, h);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer + path, int(), DESCRIPTOR_SAVE_2_START_DELAY, DESCRIPTOR_SAVE_2_END_DELAY));
    m_completionQueue.enqueue(USBDBG_DESCRIPTOR_SAVE_CPL_1);
    command();
}

void OpenMVPluginIO::getAttribute(int attribute)
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_ATTR_READ);
    serializeLong(buffer, ATTR_READ_RESPONSE_LEN);
    serializeWord(buffer, attribute);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, ATTR_READ_RESPONSE_LEN, ATTR_READ_START_DELAY, ATTR_READ_END_DELAY));
    m_completionQueue.enqueue(USBDBG_ATTR_READ_CPL);
    command();
}

void OpenMVPluginIO::setAttribute(int attribute, int value)
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_ATTR_WRITE);
    serializeLong(buffer, int());
    serializeWord(buffer, attribute);
    serializeWord(buffer, value);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, int(), ATTR_WRITE_START_DELAY, ATTR_WRITE_END_DELAY));
    m_completionQueue.enqueue(USBDBG_ATTR_WRITE_CPL);
    command();
}

void OpenMVPluginIO::sysReset()
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_SYS_RESET);
    serializeLong(buffer, int());
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, int(), SYS_RESET_START_DELAY, SYS_RESET_END_DELAY));
    m_completionQueue.enqueue(USBDBG_SYS_RESET_CPL);
    command();
}

void OpenMVPluginIO::fbEnable(bool enabled)
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_FB_ENABLE);
    serializeLong(buffer, int());
    serializeWord(buffer, enabled ? true : false);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, int(), FB_ENABLE_START_DELAY, FB_ENABLE_END_DELAY));
    m_completionQueue.enqueue(USBDBG_FB_ENABLE_CPL);
    command();
}

void OpenMVPluginIO::jpegEnable(bool enabled)
{
    Q_UNUSED(enabled)
//  QByteArray buffer;
//  serializeByte(buffer, __USBDBG_CMD);
//  serializeByte(buffer, __USBDBG_JPEG_ENABLE);
//  serializeLong(buffer, int());
//  serializeWord(buffer, enabled ? true : false);
//  m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, int(), JPEG_ENABLE_START_DELAY, JPEG_ENABLE_END_DELAY));
//  m_completionQueue.enqueue(USBDBG_JPEG_ENABLE_CPL);
//  command();
}

void OpenMVPluginIO::getTxBuffer()
{
    QByteArray buffer;
    serializeByte(buffer, __USBDBG_CMD);
    serializeByte(buffer, __USBDBG_TX_BUF_LEN);
    serializeLong(buffer, TX_BUF_LEN_RESPONSE_LEN);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, TX_BUF_LEN_RESPONSE_LEN, TX_BUF_LEN_START_DELAY, TX_BUF_LEN_END_DELAY));
    m_completionQueue.enqueue(USBDBG_TX_BUF_LEN_CPL);
    command();
}

void OpenMVPluginIO::bootloaderStart()
{
    QByteArray buffer;
    serializeLong(buffer, __BOOTLDR_START);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, BOOTLDR_START_RESPONSE_LEN, BOOTLDR_START_START_DELAY, BOOTLDR_START_END_DELAY));
    m_completionQueue.enqueue(BOOTLDR_START_CPL);
    command();
}

void OpenMVPluginIO::bootloaderReset()
{
    QByteArray buffer;
    serializeLong(buffer, __BOOTLDR_RESET);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, int(), BOOTLDR_RESET_START_DELAY, BOOTLDR_RESET_END_DELAY));
    m_completionQueue.enqueue(BOOTLDR_RESET_CPL);
    command();
}

void OpenMVPluginIO::flashErase(int sector)
{
    QByteArray buffer;
    serializeLong(buffer, __BOOTLDR_ERASE);
    serializeLong(buffer, sector);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, int(), BOOTLDR_ERASE_START_DELAY, BOOTLDR_ERASE_END_DELAY));
    m_completionQueue.enqueue(BOOTLDR_ERASE_CPL);
    command();
}

void OpenMVPluginIO::flashWrite(const QByteArray &data)
{
    QByteArray buffer;
    serializeLong(buffer, __BOOTLDR_WRITE);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer + data, int(), BOOTLDR_WRITE_START_DELAY, BOOTLDR_WRITE_END_DELAY));
    m_completionQueue.enqueue(BOOTLDR_WRITE_CPL);
    command();
}

void OpenMVPluginIO::bootloaderQuery()
{
    QByteArray buffer;
    serializeLong(buffer, __BOOTLDR_QUERY);
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(buffer, BOOTLDR_QUERY_RESPONSE_LEN, BOOTLDR_QUERY_START_DELAY, BOOTLDR_QUERY_END_DELAY));
    m_completionQueue.enqueue(BOOTLDR_QUERY_CPL);
    command();
}

void OpenMVPluginIO::close()
{
    m_postedQueue.enqueue(OpenMVPluginSerialPortCommand(QByteArray(), int(), int(), int()));
    m_completionQueue.enqueue(CLOSE_CPL);
    command();
}

QByteArray OpenMVPluginIO::pasrsePrintData(const QByteArray &data)
{
    enum
    {
        ASCII,
        UTF_8,
        EXIT_0,
        EXIT_1
    }
    int_stateMachine = ASCII;
    QByteArray int_shiftReg = QByteArray();
    QByteArray int_frameBufferData = QByteArray();

    QByteArray buffer;

    for(int i = 0, j = data.size(); i < j; i++)
    {
        if((int_stateMachine == UTF_8) && ((data.at(i) & 0xC0) != 0x80))
        {
            int_stateMachine = ASCII;
        }

        if((int_stateMachine == EXIT_0) && ((data.at(i) & 0xFF) != 0x00))
        {
            int_stateMachine = ASCII;
        }

        switch(int_stateMachine)
        {
            case ASCII:
            {
                if(((data.at(i) & 0xE0) == 0xC0)
                || ((data.at(i) & 0xF0) == 0xE0)
                || ((data.at(i) & 0xF8) == 0xF0)
                || ((data.at(i) & 0xFC) == 0xF8)
                || ((data.at(i) & 0xFE) == 0xFC)) // UTF_8
                {
                    int_shiftReg.clear();

                    int_stateMachine = UTF_8;
                }
                else if((data.at(i) & 0xFF) == 0xFF)
                {
                    int_stateMachine = EXIT_0;
                }
                else if((data.at(i) & 0xC0) == 0x80)
                {
                    int_frameBufferData.append(data.at(i));
                }
                else if((data.at(i) & 0xFF) == 0xFE)
                {
                    int size = int_frameBufferData.size();
                    QByteArray temp;

                    for(int k = 0, l = (size / 4) * 4; k < l; k += 4)
                    {
                        int x = 0;
                        x |= (int_frameBufferData.at(k + 0) & 0x3F) << 0;
                        x |= (int_frameBufferData.at(k + 1) & 0x3F) << 6;
                        x |= (int_frameBufferData.at(k + 2) & 0x3F) << 12;
                        x |= (int_frameBufferData.at(k + 3) & 0x3F) << 18;
                        temp.append((x >> 0) & 0xFF);
                        temp.append((x >> 8) & 0xFF);
                        temp.append((x >> 16) & 0xFF);
                    }

                    if((size % 4) == 3) // 2 bytes -> 16-bits -> 24-bits sent
                    {
                        int x = 0;
                        x |= (int_frameBufferData.at(size - 3) & 0x3F) << 0;
                        x |= (int_frameBufferData.at(size - 2) & 0x3F) << 6;
                        x |= (int_frameBufferData.at(size - 1) & 0x0F) << 12;
                        temp.append((x >> 0) & 0xFF);
                        temp.append((x >> 8) & 0xFF);
                    }

                    if((size % 4) == 2) // 1 byte -> 8-bits -> 16-bits sent
                    {
                        int x = 0;
                        x |= (int_frameBufferData.at(size - 2) & 0x3F) << 0;
                        x |= (int_frameBufferData.at(size - 1) & 0x03) << 6;
                        temp.append((x >> 0) & 0xFF);
                    }

                    QPixmap pixmap = QPixmap::fromImage(QImage::fromData(temp, "JPG"));

                    if(!pixmap.isNull())
                    {
                        emit frameBufferData(pixmap);
                    }

                    int_frameBufferData.clear();
                }
                else if((data.at(i) & 0x80) == 0x00) // ASCII
                {
                    buffer.append(data.at(i));
                }

                break;
            }

            case UTF_8:
            {
                if((((int_shiftReg.at(0) & 0xE0) == 0xC0) && (int_shiftReg.size() == 1))
                || (((int_shiftReg.at(0) & 0xF0) == 0xE0) && (int_shiftReg.size() == 2))
                || (((int_shiftReg.at(0) & 0xF8) == 0xF0) && (int_shiftReg.size() == 3))
                || (((int_shiftReg.at(0) & 0xFC) == 0xF8) && (int_shiftReg.size() == 4))
                || (((int_shiftReg.at(0) & 0xFE) == 0xFC) && (int_shiftReg.size() == 5)))
                {
                    buffer.append(int_shiftReg + data.at(i));

                    int_stateMachine = ASCII;
                }

                break;
            }

            case EXIT_0:
            {
                int_stateMachine = EXIT_1;

                break;
            }

            case EXIT_1:
            {
                int_stateMachine = ASCII;

                break;
            }
        }

        int_shiftReg = int_shiftReg.append(data.at(i)).right(5);
    }

    return buffer;
}
