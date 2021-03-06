#ifndef ACTION_INFO_HPP
#define ACTION_INFO_HPP 1

#include "statevariable.hpp"
#include <QSharedDataPointer>

START_DEFINE_UPNP_NAMESPACE

class SActionInfoData;

/*! \brief Holds information about send and retrieve data for an action.
 *
 * It contains some data used by the action caller to invoke the action and the response.
 *
 * \remark Use implicit Sharing QT technology.
 *
 */
class UPNP_API CActionInfo
{
public :
  /*! Default constructor. */
  CActionInfo ();

  /*! Copy constructor. */
  CActionInfo (CActionInfo const & other);

  /*! Destructor. */
  ~CActionInfo ();

  /*! Copy operator. */
  CActionInfo& operator = (CActionInfo const & other);

  /*! Prepares the data for the action caller.
   * \param deviceUUID: The device uuid.
   * \param serviceID: The service ID that contains action.
   * \param action: The action name.
   */
  void startMessage (QString const & deviceUUID, QString const & serviceID, QString const & action);

  /*! End the message creation. */
  void endMessage ();

  /*! Add an argument.
   * \param name: The argument name.
   * \param value: The parameter value.
   */
  void addArgument (QString const & name, QString const & value);

  /*! Returns true if the action is sent without network error.
   * May be, an UPnP error can be generated by the device.
   */
  bool succeeded () const;

  /*! Returns the device uuid. */
  QString const & deviceUUID () const;

  /*! Returns the service identifier. */
  QString const & serviceID () const;

  /*! Returns the action name. */
  QString const & actionName () const;

  /*! Returns the actual message to sent. */
  QString const & message () const;

  /*! Returns the response of the QNetworkmanager. */
  QByteArray const & response () const;

  /*! Sets the success state of the QNetworkManager response. */
  void setSucceeded (bool success);

  /*! Sets the device uuid. */
  void setDeviceUUID (QString const & uuid);

  /*! Sets the service identifier. */
  void setserviceID (QString const & type);

  /*! Sets the action name. */
  void setActionName (QString const & name);

  /*! Sets the message. */
  void setMessage (QString const & message);

  /*! Sets the response. */
  void setResponse (QByteArray const & response);

private :
  QSharedDataPointer<SActionInfoData> m_d; //!< Shared data pointer.
};

} // End namespace

#endif
