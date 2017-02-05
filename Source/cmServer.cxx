/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmServer.h"
#include <iomanip>
#include <sstream>

#include "cmFileMonitor.h"
#include "cmServerConnection.h"
#include "cmServerDictionary.h"
#include "cmServerProtocol.h"
#include "cmSystemTools.h"
#include "cmVersionMacros.h"
#include "cmake.h"

#if defined(CMAKE_BUILD_WITH_CMAKE)

#include "cm_jsoncpp_reader.h"
#include "cm_jsoncpp_value.h"

#endif

#include <algorithm>
#include <fstream>
#include <iostream>

template <typename T>
static void onLoop(T* handle)
{
  auto serverBase = reinterpret_cast<cmServerBase*>(handle->data);
  serverBase->ProcessOne();
}

void on_signal(uv_signal_t* signal, int signum)
{
  auto conn = reinterpret_cast<cmServerBase*>(signal->data);
  conn->OnSignal(signum);
}

static void on_walk_to_shutdown(uv_handle_t* handle, void* arg)
{
  (void)arg;
  if (!uv_is_closing(handle))
    uv_close(handle, &cmConnection::on_close);
}

class cmServer::DebugInfo
{
public:
  DebugInfo()
    : StartTime(uv_hrtime())
  {
  }

  bool PrintStatistics = false;

  std::string OutputFile;
  uint64_t StartTime;
};

cmServer::cmServer(cmConnection* conn, bool supportExperimental)
  : cmServerBase(conn)
  , SupportExperimental(supportExperimental)
{
  // Register supported protocols:
  this->RegisterProtocol(new cmServerProtocol1_0);
}

cmServer::~cmServer()
{
  if (!this->Protocol) { // Server was never fully started!
    return;
  }

  for (cmServerProtocol* p : this->SupportedProtocols) {
    delete p;
  }
}

void cmServer::ProcessRequest(cmConnection* connection,
                              const std::string& input)
{
  Json::Reader reader;
  Json::Value value;
  if (!reader.parse(input, value)) {
    this->WriteParseError(connection, "Failed to parse JSON input.");
    return;
  }

  std::unique_ptr<DebugInfo> debug;
  Json::Value debugValue = value["debug"];
  if (!debugValue.isNull()) {
    debug = std::make_unique<DebugInfo>();
    debug->OutputFile = debugValue["dumpToFile"].asString();
    debug->PrintStatistics = debugValue["showStats"].asBool();
  }

  const cmServerRequest request(this, connection, value[kTYPE_KEY].asString(),
                                value[kCOOKIE_KEY].asString(), value);

  if (request.Type == "") {
    cmServerResponse response(request);
    response.SetError("No type given in request.");
    this->WriteResponse(connection, response, nullptr);
    return;
  }

  cmSystemTools::SetMessageCallback(reportMessage,
                                    const_cast<cmServerRequest*>(&request));
  if (this->Protocol) {
    this->Protocol->CMakeInstance()->SetProgressCallback(
      reportProgress, const_cast<cmServerRequest*>(&request));
    this->WriteResponse(connection, this->Protocol->Process(request),
                        debug.get());
  } else {
    this->WriteResponse(connection, this->SetProtocolVersion(request),
                        debug.get());
  }
}

void cmServer::RegisterProtocol(cmServerProtocol* protocol)
{
  if (protocol->IsExperimental() && !this->SupportExperimental) {
    return;
  }
  auto version = protocol->ProtocolVersion();
  assert(version.first >= 0);
  assert(version.second >= 0);
  auto it = std::find_if(this->SupportedProtocols.begin(),
                         this->SupportedProtocols.end(),
                         [version](cmServerProtocol* p) {
                           return p->ProtocolVersion() == version;
                         });
  if (it == this->SupportedProtocols.end()) {
    this->SupportedProtocols.push_back(protocol);
  }
}

void cmServer::PrintHello(cmConnection* connection) const
{
  Json::Value hello = Json::objectValue;
  hello[kTYPE_KEY] = "hello";

  Json::Value& protocolVersions = hello[kSUPPORTED_PROTOCOL_VERSIONS] =
    Json::arrayValue;

  for (auto const& proto : this->SupportedProtocols) {
    auto version = proto->ProtocolVersion();
    Json::Value tmp = Json::objectValue;
    tmp[kMAJOR_KEY] = version.first;
    tmp[kMINOR_KEY] = version.second;
    if (proto->IsExperimental()) {
      tmp[kIS_EXPERIMENTAL_KEY] = true;
    }
    protocolVersions.append(tmp);
  }

  this->WriteJsonObject(connection, hello, nullptr);
}

void cmServer::reportProgress(const char* msg, float progress, void* data)
{
  const cmServerRequest* request = static_cast<const cmServerRequest*>(data);
  assert(request);
  if (progress < 0.0 || progress > 1.0) {
    request->ReportMessage(msg, "");
  } else {
    request->ReportProgress(0, static_cast<int>(progress * 1000), 1000, msg);
  }
}

void cmServer::reportMessage(const char* msg, const char* title,
                             bool& /* cancel */, void* data)
{
  const cmServerRequest* request = static_cast<const cmServerRequest*>(data);
  assert(request);
  assert(msg);
  std::string titleString;
  if (title) {
    titleString = title;
  }
  request->ReportMessage(std::string(msg), titleString);
}

cmServerResponse cmServer::SetProtocolVersion(const cmServerRequest& request)
{
  if (request.Type != kHANDSHAKE_TYPE) {
    return request.ReportError("Waiting for type \"" + kHANDSHAKE_TYPE +
                               "\".");
  }

  Json::Value requestedProtocolVersion = request.Data[kPROTOCOL_VERSION_KEY];
  if (requestedProtocolVersion.isNull()) {
    return request.ReportError("\"" + kPROTOCOL_VERSION_KEY +
                               "\" is required for \"" + kHANDSHAKE_TYPE +
                               "\".");
  }

  if (!requestedProtocolVersion.isObject()) {
    return request.ReportError("\"" + kPROTOCOL_VERSION_KEY +
                               "\" must be a JSON object.");
  }

  Json::Value majorValue = requestedProtocolVersion[kMAJOR_KEY];
  if (!majorValue.isInt()) {
    return request.ReportError("\"" + kMAJOR_KEY +
                               "\" must be set and an integer.");
  }

  Json::Value minorValue = requestedProtocolVersion[kMINOR_KEY];
  if (!minorValue.isNull() && !minorValue.isInt()) {
    return request.ReportError("\"" + kMINOR_KEY +
                               "\" must be unset or an integer.");
  }

  const int major = majorValue.asInt();
  const int minor = minorValue.isNull() ? -1 : minorValue.asInt();
  if (major < 0) {
    return request.ReportError("\"" + kMAJOR_KEY + "\" must be >= 0.");
  }
  if (!minorValue.isNull() && minor < 0) {
    return request.ReportError("\"" + kMINOR_KEY +
                               "\" must be >= 0 when set.");
  }

  this->Protocol =
    this->FindMatchingProtocol(this->SupportedProtocols, major, minor);
  if (!this->Protocol) {
    return request.ReportError("Protocol version not supported.");
  }

  std::string errorMessage;
  if (!this->Protocol->Activate(this, request, &errorMessage)) {
    this->Protocol = CM_NULLPTR;
    return request.ReportError("Failed to activate protocol version: " +
                               errorMessage);
  }
  return request.Reply(Json::objectValue);
}

bool cmServer::Serve(std::string* errorMessage)
{
  if (this->SupportedProtocols.empty()) {
    *errorMessage =
      "No protocol versions defined. Maybe you need --experimental?";
    return false;
  }
  assert(!this->Protocol);

  return cmServerBase::Serve(errorMessage);
}

cmFileMonitor* cmServer::FileMonitor() const
{
  return fileMonitor.get();
}

void cmServer::WriteJsonObject(cmConnection* connection,
                               const Json::Value& jsonValue,
                               const DebugInfo* debug) const
{
  Json::FastWriter writer;

  auto beforeJson = uv_hrtime();
  std::string result = writer.write(jsonValue);

  if (debug) {
    Json::Value copy = jsonValue;
    if (debug->PrintStatistics) {
      Json::Value stats = Json::objectValue;
      auto endTime = uv_hrtime();

      stats["jsonSerialization"] = double(endTime - beforeJson) / 1000000.0;
      stats["totalTime"] = double(endTime - debug->StartTime) / 1000000.0;
      stats["size"] = static_cast<int>(result.size());
      if (!debug->OutputFile.empty()) {
        stats["dumpFile"] = debug->OutputFile;
      }

      copy["zzzDebug"] = stats;

      result = writer.write(copy); // Update result to include debug info
    }

    if (!debug->OutputFile.empty()) {
      std::ofstream myfile;
      myfile.open(debug->OutputFile);
      myfile << result;
      myfile.close();
    }
  }

  connection->WriteData(std::string("\n") + kSTART_MAGIC + std::string("\n") +
                        result + kEND_MAGIC + std::string("\n"));
}

cmServerProtocol* cmServer::FindMatchingProtocol(
  const std::vector<cmServerProtocol*>& protocols, int major, int minor)
{
  cmServerProtocol* bestMatch = nullptr;
  for (auto protocol : protocols) {
    auto version = protocol->ProtocolVersion();
    if (major != version.first) {
      continue;
    }
    if (minor == version.second) {
      return protocol;
    }
    if (!bestMatch || bestMatch->ProtocolVersion().second < version.second) {
      bestMatch = protocol;
    }
  }
  return minor < 0 ? bestMatch : nullptr;
}

void cmServer::WriteProgress(const cmServerRequest& request, int min,
                             int current, int max,
                             const std::string& message) const
{
  assert(min <= current && current <= max);
  assert(message.length() != 0);

  Json::Value obj = Json::objectValue;
  obj[kTYPE_KEY] = kPROGRESS_TYPE;
  obj[kREPLY_TO_KEY] = request.Type;
  obj[kCOOKIE_KEY] = request.Cookie;
  obj[kPROGRESS_MESSAGE_KEY] = message;
  obj[kPROGRESS_MINIMUM_KEY] = min;
  obj[kPROGRESS_MAXIMUM_KEY] = max;
  obj[kPROGRESS_CURRENT_KEY] = current;

  this->WriteJsonObject(request.Connection, obj, nullptr);
}

void cmServer::WriteMessage(const cmServerRequest& request,
                            const std::string& message,
                            const std::string& title) const
{
  if (message.empty()) {
    return;
  }

  Json::Value obj = Json::objectValue;
  obj[kTYPE_KEY] = kMESSAGE_TYPE;
  obj[kREPLY_TO_KEY] = request.Type;
  obj[kCOOKIE_KEY] = request.Cookie;
  obj[kMESSAGE_KEY] = message;
  if (!title.empty()) {
    obj[kTITLE_KEY] = title;
  }

  WriteJsonObject(request.Connection, obj, nullptr);
}

void cmServer::WriteParseError(cmConnection* connection,
                               const std::string& message) const
{
  Json::Value obj = Json::objectValue;
  obj[kTYPE_KEY] = kERROR_TYPE;
  obj[kERROR_MESSAGE_KEY] = message;
  obj[kREPLY_TO_KEY] = "";
  obj[kCOOKIE_KEY] = "";

  this->WriteJsonObject(connection, obj, nullptr);
}

void cmServer::WriteSignal(cmConnection* connection, const std::string& name,
                           const Json::Value& data) const
{
  assert(data.isObject());
  Json::Value obj = data;
  obj[kTYPE_KEY] = kSIGNAL_TYPE;
  obj[kREPLY_TO_KEY] = "";
  obj[kCOOKIE_KEY] = "";
  obj[kNAME_KEY] = name;

  WriteJsonObject(connection, obj, nullptr);
}

void cmServer::WriteResponse(cmConnection* connection,
                             const cmServerResponse& response,
                             const DebugInfo* debug) const
{
  assert(response.IsComplete());

  Json::Value obj = response.Data();
  obj[kCOOKIE_KEY] = response.Cookie;
  obj[kTYPE_KEY] = response.IsError() ? kERROR_TYPE : kREPLY_TYPE;
  obj[kREPLY_TO_KEY] = response.Type;
  if (response.IsError()) {
    obj[kERROR_MESSAGE_KEY] = response.ErrorMessage();
  }

  this->WriteJsonObject(connection, obj, debug);
}

void cmServer::OnConnected(cmConnection* connection)
{
  PrintHello(connection);
}

void cmServer::OnServeStart()
{
  cmServerBase::OnServeStart();
  fileMonitor = std::make_shared<cmFileMonitor>(GetLoop());
}

void cmServer::OnServeStop(std::string* pString)
{
  cmServerBase::OnServeStop(pString);
  if (fileMonitor) {
    fileMonitor->StopMonitoring();
    fileMonitor.reset();
  }
}

bool cmServerBase::StartServeThread()
{
  ServeThread = std::thread([&] {
    std::string error;
    Serve(&error);
  });
  return true;
}

bool cmServerBase::Serve(std::string* errorMessage)
{
  errorMessage->clear();

  uv_signal_init(&Loop, &this->SIGINTHandler);
  uv_signal_init(&Loop, &this->SIGHUPHandler);

  this->SIGINTHandler.data = this;
  this->SIGHUPHandler.data = this;

  uv_signal_start(&this->SIGINTHandler, &on_signal, SIGINT);
  uv_signal_start(&this->SIGHUPHandler, &on_signal, SIGHUP);

  uv_check_init(&Loop, &cbHandle);
  cbHandle.data = this;
  uv_check_start(&cbHandle, &onLoop);

  uv_prepare_init(&Loop, &cbPrepareHandle);
  cbPrepareHandle.data = this;
  uv_prepare_start(&cbPrepareHandle, &onLoop);

  OnServeStart();

  for (auto& connection : Connections) {
    if (!connection->OnServeStart(errorMessage))
      return false;
  }

  if (uv_run(&Loop, UV_RUN_DEFAULT) != 0) {
    *errorMessage = "Internal Error: Event loop stopped in unclean state.";
    OnServeStop(errorMessage);
    return false;
  }

  for (auto& connection : Connections) {
    if (!connection->OnServeStop(errorMessage))
      return false;
  }

  OnServeStop(errorMessage);

  return true;
}

void cmServerBase::OnConnected(cmConnection*)
{
}

void cmServerBase::OnDisconnect()
{
}

void cmServerBase::OnServeStart()
{
  uv_signal_start(&this->SIGINTHandler, &on_signal, SIGINT);
  uv_signal_start(&this->SIGHUPHandler, &on_signal, SIGHUP);
  uv_prepare_start(&cbPrepareHandle, &onLoop);
  uv_check_start(&cbHandle, &onLoop);
}

void cmServerBase::OnServeStop(std::string* pString)
{
  if (!uv_is_closing((const uv_handle_t*)&this->SIGINTHandler))
    uv_signal_stop(&this->SIGINTHandler);
  if (!uv_is_closing((const uv_handle_t*)&this->SIGHUPHandler))
    uv_signal_stop(&this->SIGHUPHandler);
  uv_prepare_stop(&cbPrepareHandle);
  uv_check_stop(&cbHandle);
}

bool cmServerBase::OnSignal(int signum)
{
  return false;
}

cmServerBase::cmServerBase(cmConnection* connection)
{
  uv_loop_init(&Loop);
  uv_async_init(&Loop, &WakeupLoop, 0);

  uv_signal_init(&Loop, &this->SIGINTHandler);
  uv_signal_init(&Loop, &this->SIGHUPHandler);

  this->SIGINTHandler.data = this;
  this->SIGHUPHandler.data = this;

  uv_check_init(&Loop, &cbHandle);
  cbHandle.data = this;

  uv_prepare_init(&Loop, &cbPrepareHandle);
  cbPrepareHandle.data = this;

  AddNewConnection(connection);
}

cmServerBase::~cmServerBase()
{
  Connections.clear();

  uv_stop(&Loop);
  uv_async_send(&WakeupLoop);
  uv_walk(&Loop, on_walk_to_shutdown, NULL);

  ServeThread.join();
}

void cmServerBase::AddNewConnection(cmConnection* ownedConnection)
{
  Connections.emplace_back(ownedConnection);
  ownedConnection->SetServer(this);
}

void cmServerBase::ProcessOne()
{
  for (auto& connection : Connections)
    connection->PopOne();
}

uv_loop_t* cmServerBase::GetLoop()
{
  return &Loop;
}
