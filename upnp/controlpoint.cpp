#include "controlpoint.hpp"
#include "initialdiscovery.hpp"
#include "xmlhaction.hpp"
#include "actioninfo.hpp"
#include "multicastsocket.hpp"
#include "unicastsocket.hpp"

USING_UPNP_NAMESPACE

/*! Use an empty list. */
QList<CControlPoint::TArgValue> CControlPoint::noArgs = QList<CControlPoint::TArgValue> ();

CControlPoint::CControlPoint (QObject* parent) : QObject (parent)
{
#ifdef QT_DEBUG
  m_discoveryPause /= 3;
#endif
  m_done = initialize ();
}

CControlPoint::~CControlPoint ()
{
  if (!m_closing)
  {
    close ();
  }
}

bool CControlPoint::initialize ()
{
  int const cUnicastSockets   = sizeof (m_unicastSockets) / sizeof (CUnicastSocket*);
  bool done                   = false;
  m_multicastSockets[McpIPV4] = nullptr;
  m_multicastSockets[McpIPV6] = nullptr;
  for (int k = 0; k < cUnicastSockets; ++k)
  {
    m_unicastSockets[k] = nullptr;
  }

  m_multicastSockets[McpIPV4] = new CMulticastSocket (this);
  connect (m_multicastSockets[McpIPV4], SIGNAL(readyRead()), this, SLOT(readDatagrams()));
  m_multicastSockets[McpIPV6] = nullptr;
  if (m_multicastSockets[McpIPV4]->initialize (m_upnpMulticastAddr, m_upnpMulticastPort))
  {
    done                        = true;
    m_multicastSockets[McpIPV6] = new CMulticastSocket (this);
    connect (m_multicastSockets[McpIPV6], SIGNAL(readyRead()), this, SLOT(readDatagrams()));
    m_multicastSockets[McpIPV6]->initialize6 (m_upnpMulticastAddr6, m_upnpMulticastPort);
    QHostAddress addresses[] = { QHostAddress ("127.0.0.1"), CUpnpSocket::localHostAddress ()};
    for (int k = 0; k < cUnicastSockets; ++k)
    {
      m_unicastSockets[k] = new CUnicastSocket (this);
      connect (m_unicastSockets[k], SIGNAL(readyRead()), this, SLOT(readDatagrams()));
      int iAddr = k % (sizeof (addresses) / sizeof (QHostAddress));
      done     &= m_unicastSockets[k]->bind (addresses[iAddr]) != 0;
    }

    if (done)
    {
      connect (m_devices.httpServer (), SIGNAL(eventReady(QString const &)), this, SLOT(updateEventVars(QString const &)));
    }
  }

  return done;
}

bool CControlPoint::avDiscover ()
{
  bool success = false;
  if (!m_closing)
  {
    char const * urns[] = { "urn:schemas-upnp-org:device:MediaServer:1",
                            "urn:schemas-upnp-org:device:MediaRenderer:1",
                            "upnp:rootdevice",
                          };

    int cUnicastSockets = sizeof (m_unicastSockets) / sizeof (CUnicastSocket*);
    int cDiscoveries    = m_discoveryRetryCount * cUnicastSockets;
    int iDiscovery      = 0;
    int pauseMin        = m_discoveryPause / 3;
    for (int iRetry = 0; iRetry < m_discoveryRetryCount; ++iRetry)
    {
      for (int k = 0; k < cUnicastSockets; ++k)
      {
        if (m_unicastSockets[k] != nullptr)
        {
          int               pause = qrand () % (m_discoveryPause - pauseMin) + pauseMin; // Choose a pause between m_discoveryPause / 4 and m_discoveryPause
          CInitialDiscovery initDiscovery (m_unicastSockets[k], m_upnpMulticastAddr, m_upnpMulticastPort, pause);
          char const *      urn = urns[k % (sizeof (urns) / sizeof (char const *))];
          emit searched (urn, ++iDiscovery, cDiscoveries);
          success |= initDiscovery.discover (false, urn);
        }
      }
    }
  }
  return success;
}

bool CControlPoint::discover (char const * nt)
{
  bool success     = false;
  int cDiscoveries = m_discoveryRetryCount * sizeof (m_unicastSockets) / sizeof (CUnicastSocket*);
  int iDiscovery   = 0;
  int pauseMin     = m_discoveryPause / 3;
  for (int iRetry = 0; iRetry < m_discoveryRetryCount; ++iRetry)
  {
    for (int k = 0; k < 2; ++k)
    {
      if (m_unicastSockets[k] != nullptr)
      {
        int               pause = qrand () % (m_discoveryPause - pauseMin) + pauseMin;
        CInitialDiscovery initDiscovery (m_unicastSockets[k], m_upnpMulticastAddr, m_upnpMulticastPort, pause);
        emit searched (nt, ++iDiscovery, cDiscoveries);
        success |= initDiscovery.discover (false, nt);
      }
    }
  }

  return success;
}

void CControlPoint::readDatagrams ()
{
  CUpnpSocket*       socket    = static_cast<CUpnpSocket*>(sender ());
  QByteArray const & datagrams = socket->readDatagrams ();
  if (!datagrams.isEmpty () && datagrams.endsWith ("\r\n\r\n"))
  {
    socket->decodeDatagram ();
    newDevicesDetected ();
  }
}

void CControlPoint::newDevicesDetected ()
{
  ++m_level;
  if (extractDevicesFromNotify () != 0) // Extracts devices from UDP socket cache.
  {
    if (m_level == 1)
    {
      QStringList& lostDevices = m_devices.lostDevices ();
      for (QString const & device : lostDevices)
      {
        emit lostDevice (device);
      }

      QStringList& newDevices = m_devices.newDevices ();
      for (QString const & device : m_devices.newDevices ())
      {
        emit newDevice (device);
      }

      lostDevices.clear ();
      newDevices.clear ();
    }
  }

  --m_level;
}

void CControlPoint::renewalTimeout ()
{
  QTimer* timer = static_cast<QTimer*>(sender ());
  for (QMap<QString, TSubscriptionTimer>::const_iterator it = m_subcriptionTimers.begin (), end = m_subcriptionTimers.end (); it != end; ++it)
  {
    TSubscriptionTimer const & stimer = it.value ();
    if (timer == stimer.first)
    {
      renewSubscribe (it.key (), stimer.second);
      break;
    }
  }
}

void CControlPoint::updateEventVars (QString const & sid)
{
  if (!m_closing)
  {
    QStringList emitter;
    QPair<QString, QString> ds = m_devices.eventSender (sid);
    if (!ds.first.isEmpty () && this->contains (ds.first))
    {
      CDevice&          device         = m_devices[ds.first];
      CService&         service        = device.services ()[ds.second];
      TMStateVariables& stateVariables = service.stateVariables ();
      TMEventVars       eventVars      = m_devices.httpServer ()->vars ();
      emitter.reserve (eventVars.size () + 2);
      emitter << ds.first << ds.second;
      for (TMEventVars::const_iterator it = eventVars.cbegin (), end = eventVars.cend (); it != end; ++it)
      {
        QString const & name = it.key ();
        if (stateVariables.contains (name))
        {
          CStateVariable&     var   = stateVariables[name];
          TEventValue const & value = it.value ();
          var.setValue (value.first, value.second);
          emitter << name;
        }
      }
    }

    emit eventReady (emitter);
  }
}

void CControlPoint::networkAccessManager (QString const & deviceUUID, QNetworkReply::NetworkError errorCode, QString const & errorDesc)
{
  emit networkError (deviceUUID, errorCode, errorDesc);
}

QList<CUpnpSocket::SNDevice> CControlPoint::ndevices () const
{
  QList<CUpnpSocket::SNDevice> devices;
  int                          count = 0;
  for (CUnicastSocket* ucs : m_unicastSockets)
  {
    count += ucs->devices ().size ();
  }

  for (CMulticastSocket* mcs : m_multicastSockets)
  {
    if (mcs != nullptr)
    {
      count += mcs->devices ().size ();
    }
  }

  devices.reserve (count);

  for (CMulticastSocket* mcs : m_multicastSockets)
  {
    if (mcs != nullptr)
    {
      QList<CUpnpSocket::SNDevice> const & multicastDevices = mcs->devices ();
      for (CUpnpSocket::SNDevice const & device : multicastDevices)
      {
        devices.push_back (device);
      }

      mcs->resetDevices ();
    }
  }

  for (CUnicastSocket* ucs : m_unicastSockets)
  {
    QList<CUpnpSocket::SNDevice> const & unicastDevices = ucs->devices ();
    for (CUpnpSocket::SNDevice const & device : unicastDevices)
    {
      devices.push_back (device);
    }

    ucs->resetDevices ();
  }

  return devices;
}

int CControlPoint::extractDevicesFromNotify (int timeout)
{
  int cDevices = 0;
  if (!m_closing)
  {
    QList<CUpnpSocket::SNDevice> const & ndevs = ndevices ();
    cDevices = m_devices.extractDevicesFromNotify (ndevs, timeout);
  }

  return cDevices;
}

CActionInfo CControlPoint::invokeAction (CDevice& device, CService& service,
                                         QString const & actionName, QList<TArgValue>& args, int timeout)
{
  m_lastActionError.clear ();
  bool           success = false;
  CActionInfo    actionInfo;
  if (!m_closing)
  {
    CActionManager actionManager;
    connect (&actionManager, SIGNAL(networkError(QString const &, QNetworkReply::NetworkError, QString const &)),
             this, SLOT(networkAccessManager(QString const &, QNetworkReply::NetworkError, QString const &)));

    TMActions const & actions = service.actions (); // Action list of service
    if (actions.contains (actionName))
    { // The service has the action
      CAction const & action  = actions.value (actionName);
      QUrl            url     = device.url (); // Base url
      url.setPath (service.controlURL ()); // complete service url
      actionInfo.startMessage (device.uuid (), service.serviceType (), actionName); // Start the HTTP message with upnp format.

      success                       = true;
      TMArguments const & arguments = action.arguments (); // Argument list of action.
      for (TArgValue const & arg : args)
      {
        if (arguments.contains (arg.first))
        { // Known argument name.
          CArgument const & argument = arguments.value (arg.first); // Get the argument.
          if (argument.dir () == CArgument::In)
          { // In argument
            QString const &   relatedStateVariableName = argument.relatedStateVariable (); // Get related state variable name.
            TMStateVariables& stateVariables           = service.stateVariables (); // Get related state variable.
            if (stateVariables.contains (relatedStateVariableName))
            { // Known state variable.
              CStateVariable& stateVariable = stateVariables[relatedStateVariableName]; //Get the variable.
              stateVariable.setValue (arg.second); // Change the variable value.
              actionInfo.addArgument (arg.first, stateVariable.type (), arg.second); // Add argument at the HTTP message
            }
            else
            {
              argStateVarRelationship (arg.first, relatedStateVariableName, actionName, device, service);
            }
          }
        }
        else
        {
          unknownArg (arg.first, actionName, device, service);
          success = false;
          break;
        }
      }

      if (success)
      {
        actionInfo.endMessage (); // End the HTTP message.
        success = actionManager.post (device.uuid (), url, actionInfo, timeout); // Invoke the action.
        if (success)
        {
          actionInfo.setSucceeded (success);

          // Parse the action response.
          QMap<QString, QString> vars; // Response of the action.
          CXmlHAction            h (actionName, vars);
          h.parse (actionInfo.response ());
          int errorCode = h.errorCode ();
          if (errorCode != 0)
          {
            emit upnpError (errorCode, h.errorDesc ());
          }
          else
          {
            TMArguments const & arguments = action.arguments ();
            // Update the out arguments value.
            for (QList<TArgValue>::iterator it = args.begin (), end = args.end (); it != end; ++it)
            {
              TArgValue& arg = *it;
              if (arguments.contains (arg.first))
              { // Known argument.
                CArgument const & argument                 = arguments.value (arg.first);
                QString const &   relatedStateVariableName = argument.relatedStateVariable ();
                TMStateVariables& stateVariables           = service.stateVariables ();
                if (stateVariables.contains (relatedStateVariableName))
                { // Known state variable.
                  CStateVariable& stateVariable = stateVariables[relatedStateVariableName];
                  if (argument.dir () == CArgument::Out)
                  { // Argument out direction.
                    QString value = vars.value (arg.first);
                    stateVariable.setValue (value); // Update the related state variable.
                    stateVariable.constraints ().clear (); // Constraints are used only from event response.
                    arg.second = value; // Update the argument value.
                  }
                }
                else
                {
                  argStateVarRelationship (arg.first, relatedStateVariableName, actionName, device, service);
                }
              }
              else
              {
                unknownArg (arg.first, actionName, device, service);
              }
            }
          }
        }
      }
    }
    else
    {
      unknownAction (actionName, device, service);
    }
  }

  return actionInfo;
}

CActionInfo CControlPoint::invokeAction (QString const & deviceUUID, QString const & serviceID,
                                         QString const & actionName, QList<TArgValue>& args, int timeout)
{
  CActionInfo actionInfo;
  if (!deviceUUID.isEmpty () && m_devices.contains (deviceUUID))
  {
    CDevice&    device   = m_devices[deviceUUID];
    TMServices& services = device.services ();
    if (services.contains (serviceID))
    {
      CService& service = services[serviceID];
      actionInfo        = invokeAction (device, service, actionName, args, timeout);
    }
    else
    {
      m_lastActionError.clear ();
      m_lastActionError.setString (ErrorDeviceUUID, deviceUUID);
      m_lastActionError.setString (ErrorServiceTypeOrID, serviceID);
    }
  }
  else
  {
    m_lastActionError.clear ();
    m_lastActionError.setString (ErrorDeviceUUID, deviceUUID);
  }

  return actionInfo;
}

CActionInfo CControlPoint::invokeAction (QString const & deviceUUID,
                QString const & actionName, QList<TArgValue>& args, int timeout)
{
  CActionInfo actionInfo;
  if (!deviceUUID.isEmpty () && m_devices.contains (deviceUUID))
  {
    CDevice&    device   = m_devices[deviceUUID];
    TMServices& services = device.services ();
    for (TMServices::iterator its = services.begin (), end = services.end (); its != end && !actionInfo.succeeded (); ++its)
    {
      CService& service = *its;
      actionInfo        = invokeAction (device, service, actionName, args, timeout);
    }
  }
  else
  {
    m_lastActionError.clear ();
    m_lastActionError.setString (ErrorDeviceUUID, deviceUUID);
  }

  return actionInfo;
}

CStateVariable CControlPoint::stateVariable (QString const & deviceUUID, QString const & serviceID, QString const & name) const
{
  CStateVariable var;
  if (!deviceUUID.isEmpty ())
  {
    CDevice const & device = this->device (deviceUUID);
    var                    = device.stateVariable (name, serviceID);
  }

  return var;
}

bool CControlPoint::subscribe (QString const & deviceUUID, int renewalDelay, int requestTimeout)
{
  bool success = false;
  if (!deviceUUID.isEmpty () && m_devices.contains (deviceUUID))
  {
    CDevice& device = m_devices[deviceUUID];
    success         = m_devices.subscribe (device, renewalDelay, requestTimeout);
    if (success)
    {
      QTimer* timer = new QTimer;
      timer->setInterval (renewalDelay * 1000 - m_renewalGard);
      timer->setSingleShot (true);
      timer->start ();
      connect (timer, SIGNAL(timeout()), this, SLOT(renewalTimeout()));
      m_subcriptionTimers.insert (deviceUUID, TSubscriptionTimer (timer, requestTimeout));
    }
  }

  return success;
}

void CControlPoint::unsubscribe (QString const & deviceUUID, int requestTimeout)
{
  if (!deviceUUID.isEmpty () && m_devices.contains (deviceUUID))
  {
    QTimer* timer = m_subcriptionTimers.value (deviceUUID).first;
    if (timer != nullptr)
    {
      delete timer;
      m_subcriptionTimers.remove (deviceUUID);
    }

    CDevice& device = m_devices[deviceUUID];
    m_devices.unsubscribe (device, requestTimeout);
  }
}

void CControlPoint::renewSubscribe (QString const & deviceUUID, int requestTimeout)
{
  if (!deviceUUID.isEmpty () && m_devices.contains (deviceUUID))
  {
    CDevice& device = m_devices[deviceUUID];
    m_devices.renewSubscribe (device, requestTimeout);
    QTimer* timer = m_subcriptionTimers.value (deviceUUID).first;
    timer->start ();
  }
}

QStringList CControlPoint::devices (CDevice::EType type) const
{
  QStringList deviceUUIDs;
  for (TMDevices::const_iterator it = m_devices.begin (); it != m_devices.end (); ++it)
  {
    CDevice const & device = *it;
    if (device.type () == type)
    {
      deviceUUIDs << device.uuid ();
    }
  }

  return deviceUUIDs;
}

bool CControlPoint::hasDevice (CDevice::EType type) const
{
  bool success = false;
  for (TMDevices::const_iterator it = m_devices.begin (); it != m_devices.end (); ++it)
  {
    CDevice const & device = *it;
    if (device.type () == type)
    {
      success = true;
      break;
    }
  }

  return success;
}

QStringList CControlPoint::renderers () const
{
  return devices (CDevice::MediaRenderer);
}

QStringList CControlPoint::servers () const
{
  return devices (CDevice::MediaServer);
}

bool CControlPoint::hasRenderer () const
{
  return hasDevice (CDevice::MediaRenderer);
}

bool CControlPoint::hasServer () const
{
  return hasDevice (CDevice::MediaServer);
}

QString CControlPoint::playlistURI (QString const & name) const
{
  QString             uri;
  CHTTPServer const * server = httpServer ();
  if (server != nullptr)
  {
    uri = server->playlistURI (name);
  }

  return uri;
}

int CControlPoint::setPlaylistContent (QList<CDidlItem::TPlaylistElem> const & items, CDidlItem::EPlaylistFormat format)
{
  int           size   = 0;
  CHTTPServer * server = httpServer ();
  if (server != nullptr)
  {
    QByteArray content = CDidlItem::buildsPlaylist (items, format);
    server->setPlaylistContent (content);
    size = content.size ();
  }

  return size;
}

void CControlPoint::setPlaylistName (QString const & name)
{
  CHTTPServer* server = httpServer ();
  if (server != nullptr)
  {
    server->setPlaylistName (name);
  }
}

QString CControlPoint::playlistName () const
{
  QString             name;
  CHTTPServer const * server = httpServer ();
  if (server != nullptr)
  {
    name = server->playlistName ();
  }

  return name;
}

void CControlPoint::clearPlaylist ()
{
  CHTTPServer* server = httpServer ();
  if (server != nullptr)
  {
    server->clearPlaylist ();
  }
}

QList<QString> CControlPoint::subscribedDevices () const
{
  QStringList devices;
  for (TMDevices::const_iterator it = m_devices.begin (), end = m_devices.end (); it != end; ++it)
  {
    CDevice const & device = it.value ();
    if (device.hasSubscribed ())
    {
      devices << it.key ();
    }
  }

  return devices;
}

void CControlPoint::removeDevice (QString const & uuid)
{
  m_devices.removeDevice (uuid);
}

void CControlPoint::initActionError (QString const & action, CDevice const & device, CService const & service)
{
  m_lastActionError.setString (ErrorDeviceUUID, device.uuid ());
  m_lastActionError.setString (ErrorServiceTypeOrID, service.serviceType ());
  m_lastActionError.setString (ErrorActionName, action);
}

void CControlPoint::initArgError (QString const & arg, QString const & action, CDevice const & device, CService const & service)
{
  initActionError (action,  device, service);
  m_lastActionError.setString (ErrorArgName, arg);
}

void CControlPoint::unknownAction (QString const & action, CDevice const & device, CService const & service)
{
  initActionError (action,  device, service);
  m_lastActionError.setString (ErrorActionString,
                               QString ("Unknown action: %1 for device : %2 and service: %3")
                                        .arg (action).arg (device.uuid ()).arg (service.serviceType ()));
}

void CControlPoint::unknownArg (QString const & action, QString const & arg, CDevice const & device, CService const & service)
{
  initArgError (action, arg, device, service);
  m_lastActionError.setString (ErrorActionString,
                               QString ("Unknown argument: %1 for action: %2 for device : %3 and service: %4")
                               .arg (arg).arg (action).arg (device.uuid ()).arg (service.serviceType ()));
}

void CControlPoint::argStateVarRelationship (QString const & arg, QString const & stateVar, QString const & action,
                    CDevice const & device, CService const & service)
{
  initArgError (action, arg, device, service);
  m_lastActionError.setString (ErrorRelatedStateVar, stateVar);
  m_lastActionError.setString (ErrorActionString,
                               QString ("Wrong relationship between argument and state variable: %1 for action: %2 for device : %3 and service: %4")
                                        .arg (arg).arg (action).arg (device.uuid ()).arg (service.serviceType ()));
}

void CControlPoint::unknownService (QString const & uuid, QString const & id)
{
  m_lastActionError.setString (ErrorDeviceUUID, uuid);
  m_lastActionError.setString (ErrorServiceTypeOrID, id);
  m_lastActionError.setString (ErrorActionString,
                               QString ("Wrong service identifier: %1 for device: %2").arg (uuid).arg (id));

}

void CControlPoint::unknownDevice (QString const & uuid)
{
  m_lastActionError.setString (ErrorDeviceUUID, uuid);
  m_lastActionError.setString (ErrorActionString, QString ("Wrong device uuid: %1").arg (uuid));
}

QNetworkAccessManager* CControlPoint::networkAccessManager () const
{
  return m_devices.networkAccessManager ();
}

void CControlPoint::close ()
{
  m_closing         = true;
  QStringList uuids = subscribedDevices ();
  if (!uuids.isEmpty ())
  {
    for (QString const & uuid : uuids)
    {
      unsubscribe (uuid);
    }
  }

  for (unsigned k = 0; k < sizeof (m_unicastSockets) / sizeof (CUnicastSocket*); ++k)
  {
    m_unicastSockets[k]->close ();
  }

  for (unsigned k = 0; k < sizeof (m_multicastSockets) / sizeof (CMulticastSocket*); ++k)
  {
    m_multicastSockets[k]->close ();
  }
}
