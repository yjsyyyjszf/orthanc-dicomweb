/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2018 Osimis S.A., Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Affero General Public License for more details.
 * 
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "DicomWebClient.h"

#include "Plugin.h"
#include "DicomWebServers.h"

#include <json/reader.h>
#include <list>
#include <set>
#include <boost/lexical_cast.hpp>

#include <Core/ChunkedBuffer.h>
#include <Core/Toolbox.h>


static void AddInstance(std::list<std::string>& target,
                        const Json::Value& instance)
{
  if (instance.type() != Json::objectValue ||
      !instance.isMember("ID") ||
      instance["ID"].type() != Json::stringValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }
  else
  {
    target.push_back(instance["ID"].asString());
  }
}


static bool GetSequenceSize(size_t& result,
                            const Json::Value& answer,
                            const std::string& tag,
                            bool isMandatory,
                            const std::string& server)
{
  const Json::Value* value = NULL;

  std::string upper, lower;
  Orthanc::Toolbox::ToUpperCase(upper, tag);
  Orthanc::Toolbox::ToLowerCase(lower, tag);
  
  if (answer.isMember(upper))
  {
    value = &answer[upper];
  }
  else if (answer.isMember(lower))
  {
    value = &answer[lower];
  }
  else if (isMandatory)
  {
    OrthancPlugins::Configuration::LogError("The STOW-RS JSON response from DICOMweb server " + server + 
                                            " does not contain the mandatory tag " + upper);
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }
  else
  {
    return false;
  }

  if (value->type() != Json::objectValue ||
      !value->isMember("Value") ||
      (*value) ["Value"].type() != Json::arrayValue)
  {
    OrthancPlugins::Configuration::LogError("Unable to parse STOW-RS JSON response from DICOMweb server " + server);
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  result = (*value) ["Value"].size();
  return true;
}



static void ParseStowRequest(std::list<std::string>& instances /* out */,
                             std::map<std::string, std::string>& httpHeaders /* out */,
                             std::map<std::string, std::string>& queryArguments /* out */,
                             const OrthancPluginHttpRequest* request /* in */)
{
  static const char* RESOURCES = "Resources";
  static const char* HTTP_HEADERS = "HttpHeaders";
  static const char* QUERY_ARGUMENTS = "Arguments";

  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  Json::Value body;
  Json::Reader reader;
  if (!reader.parse(request->body, request->body + request->bodySize, body) ||
      body.type() != Json::objectValue ||
      !body.isMember(RESOURCES) ||
      body[RESOURCES].type() != Json::arrayValue)
  {
    OrthancPlugins::Configuration::LogError("A request to the DICOMweb STOW-RS client must provide a JSON object "
                                            "with the field \"" + std::string(RESOURCES) + 
                                            "\" containing an array of resources to be sent");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }

  OrthancPlugins::ParseAssociativeArray(queryArguments, body, QUERY_ARGUMENTS);
  OrthancPlugins::ParseAssociativeArray(httpHeaders, body, HTTP_HEADERS);

  Json::Value& resources = body[RESOURCES];

  // Extract information about all the child instances
  for (Json::Value::ArrayIndex i = 0; i < resources.size(); i++)
  {
    if (resources[i].type() != Json::stringValue)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
    }

    std::string resource = resources[i].asString();
    if (resource.empty())
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource);
    }

    // Test whether this resource is an instance
    Json::Value tmp;
    if (OrthancPlugins::RestApiGet(tmp, context, "/instances/" + resource, false))
    {
      AddInstance(instances, tmp);
    }
    // This was not an instance, successively try with series/studies/patients
    else if ((OrthancPlugins::RestApiGet(tmp, context, "/series/" + resource, false) &&
              OrthancPlugins::RestApiGet(tmp, context, "/series/" + resource + "/instances", false)) ||
             (OrthancPlugins::RestApiGet(tmp, context, "/studies/" + resource, false) &&
              OrthancPlugins::RestApiGet(tmp, context, "/studies/" + resource + "/instances", false)) ||
             (OrthancPlugins::RestApiGet(tmp, context, "/patients/" + resource, false) &&
              OrthancPlugins::RestApiGet(tmp, context, "/patients/" + resource + "/instances", false)))
    {
      if (tmp.type() != Json::arrayValue)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }

      for (Json::Value::ArrayIndex j = 0; j < tmp.size(); j++)
      {
        AddInstance(instances, tmp[j]);
      }
    }
    else
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource);
    }   
  }
}


static void SendStowChunks(const Orthanc::WebServiceParameters& server,
                           const std::map<std::string, std::string>& httpHeaders,
                           const std::map<std::string, std::string>& queryArguments,
                           const std::string& boundary,
                           Orthanc::ChunkedBuffer& chunks,
                           size_t& countInstances,
                           bool force)
{
  unsigned int maxInstances = OrthancPlugins::Configuration::GetUnsignedIntegerValue("StowMaxInstances", 10);
  size_t maxSize = static_cast<size_t>(OrthancPlugins::Configuration::GetUnsignedIntegerValue("StowMaxSize", 10)) * 1024 * 1024;

  if ((force && countInstances > 0) ||
      (maxInstances != 0 && countInstances >= maxInstances) ||
      (maxSize != 0 && chunks.GetNumBytes() >= maxSize))
  {
    chunks.AddChunk("\r\n--" + boundary + "--\r\n");

    std::string body;
    chunks.Flatten(body);

    OrthancPlugins::MemoryBuffer answerBody(OrthancPlugins::Configuration::GetContext());
    std::map<std::string, std::string> answerHeaders;

    std::string uri;
    OrthancPlugins::UriEncode(uri, "studies", queryArguments);

    OrthancPlugins::CallServer(answerBody, answerHeaders, server, OrthancPluginHttpMethod_Post,
                               httpHeaders, uri, body);

    Json::Value response;
    Json::Reader reader;
    bool success = reader.parse(reinterpret_cast<const char*>((*answerBody)->data),
                                reinterpret_cast<const char*>((*answerBody)->data) + (*answerBody)->size, response);
    answerBody.Clear();

    if (!success ||
        response.type() != Json::objectValue ||
        !response.isMember("00081199"))
    {
      OrthancPlugins::Configuration::LogError("Unable to parse STOW-RS JSON response from DICOMweb server " + server.GetUrl());
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
    }

    size_t size;
    if (!GetSequenceSize(size, response, "00081199", true, server.GetUrl()) ||
        size != countInstances)
    {
      OrthancPlugins::Configuration::LogError("The STOW-RS server was only able to receive " + 
                                              boost::lexical_cast<std::string>(size) + " instances out of " +
                                              boost::lexical_cast<std::string>(countInstances));
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
    }

    if (GetSequenceSize(size, response, "00081198", false, server.GetUrl()) &&
        size != 0)
    {
      OrthancPlugins::Configuration::LogError("The response from the STOW-RS server contains " + 
                                              boost::lexical_cast<std::string>(size) + 
                                              " items in its Failed SOP Sequence (0008,1198) tag");
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);    
    }

    if (GetSequenceSize(size, response, "0008119A", false, server.GetUrl()) &&
        size != 0)
    {
      OrthancPlugins::Configuration::LogError("The response from the STOW-RS server contains " + 
                                              boost::lexical_cast<std::string>(size) + 
                                              " items in its Other Failures Sequence (0008,119A) tag");
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);    
    }

    countInstances = 0;
  }
}


void StowClient(OrthancPluginRestOutput* output,
                const char* /*url*/,
                const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  if (request->groupsCount != 1)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
  }

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "POST");
    return;
  }

  Orthanc::WebServiceParameters server(OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]));

  std::string boundary;

  {
    char* uuid = OrthancPluginGenerateUuid(context);
    try
    {
      boundary.assign(uuid);
    }
    catch (...)
    {
      OrthancPluginFreeString(context, uuid);
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NotEnoughMemory);
    }

    OrthancPluginFreeString(context, uuid);
  }

  std::string mime = "multipart/related; type=application/dicom; boundary=" + boundary;

  std::map<std::string, std::string> queryArguments;
  std::map<std::string, std::string> httpHeaders;
  httpHeaders["Accept"] = "application/dicom+json";
  httpHeaders["Expect"] = "";
  httpHeaders["Content-Type"] = mime;

  std::list<std::string> instances;
  ParseStowRequest(instances, httpHeaders, queryArguments, request);

  OrthancPlugins::Configuration::LogInfo("Sending " + boost::lexical_cast<std::string>(instances.size()) +
                                         " instances using STOW-RS to DICOMweb server: " + server.GetUrl());

  Orthanc::ChunkedBuffer chunks;
  size_t countInstances = 0;

  for (std::list<std::string>::const_iterator it = instances.begin(); it != instances.end(); ++it)
  {
    OrthancPlugins::MemoryBuffer dicom(context);
    if (dicom.RestApiGet("/instances/" + *it + "/file", false))
    {
      chunks.AddChunk("\r\n--" + boundary + "\r\n" +
                      "Content-Type: application/dicom\r\n" +
                      "Content-Length: " + boost::lexical_cast<std::string>(dicom.GetSize()) +
                      "\r\n\r\n");
      chunks.AddChunk(dicom.GetData(), dicom.GetSize());
      countInstances ++;

      SendStowChunks(server, httpHeaders, queryArguments, boundary, chunks, countInstances, false);
    }
  }

  SendStowChunks(server, httpHeaders, queryArguments, boundary, chunks, countInstances, true);

  std::string answer = "{}\n";
  OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), "application/json");
}


static bool GetStringValue(std::string& target,
                           const Json::Value& json,
                           const std::string& key)
{
  if (json.type() != Json::objectValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }
  else if (!json.isMember(key))
  {
    target.clear();
    return false;
  }
  else if (json[key].type() != Json::stringValue)
  {
    OrthancPlugins::Configuration::LogError("The field \"" + key + "\" in a JSON object should be a string");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }
  else
  {
    target = json[key].asString();
    return true;
  }
}


void GetFromServer(OrthancPluginRestOutput* output,
                   const char* /*url*/,
                   const OrthancPluginHttpRequest* request)
{
  static const char* URI = "Uri";
  static const char* HTTP_HEADERS = "HttpHeaders";
  static const char* GET_ARGUMENTS = "Arguments";

  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "POST");
    return;
  }

  Orthanc::WebServiceParameters server(OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]));

  std::string tmp;
  Json::Value body;
  Json::Reader reader;
  if (!reader.parse(request->body, request->body + request->bodySize, body) ||
      body.type() != Json::objectValue ||
      !GetStringValue(tmp, body, URI))
  {
    OrthancPlugins::Configuration::LogError("A request to the DICOMweb client must provide a JSON object "
                                            "with the field \"Uri\" containing the URI of interest");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }

  std::map<std::string, std::string> getArguments;
  OrthancPlugins::ParseAssociativeArray(getArguments, body, GET_ARGUMENTS);

  std::string uri;
  OrthancPlugins::UriEncode(uri, tmp, getArguments);

  std::map<std::string, std::string> httpHeaders;
  OrthancPlugins::ParseAssociativeArray(httpHeaders, body, HTTP_HEADERS);

  OrthancPlugins::MemoryBuffer answerBody(context);
  std::map<std::string, std::string> answerHeaders;
  OrthancPlugins::CallServer(answerBody, answerHeaders, server, OrthancPluginHttpMethod_Get, httpHeaders, uri, "");

  std::string contentType = "application/octet-stream";

  for (std::map<std::string, std::string>::const_iterator
         it = answerHeaders.begin(); it != answerHeaders.end(); ++it)
  {
    std::string key = it->first;
    Orthanc::Toolbox::ToLowerCase(key);

    if (key == "content-type")
    {
      contentType = it->second;
    }
    else if (key == "transfer-encoding")
    {
      // Do not forward this header
    }
    else
    {
      OrthancPluginSetHttpHeader(context, output, it->first.c_str(), it->second.c_str());
    }
  }

  OrthancPluginAnswerBuffer(context, output, 
                            reinterpret_cast<const char*>(answerBody.GetData()),
                            answerBody.GetSize(), contentType.c_str());
}



static void RetrieveFromServerInternal(std::set<std::string>& instances,
                                       const Orthanc::WebServiceParameters& server,
                                       const std::map<std::string, std::string>& httpHeaders,
                                       const std::map<std::string, std::string>& getArguments,
                                       const Json::Value& resource)
{
  static const std::string STUDY = "Study";
  static const std::string SERIES = "Series";
  static const std::string INSTANCE = "Instance";
  static const std::string MULTIPART_RELATED = "multipart/related";
  static const std::string APPLICATION_DICOM = "application/dicom";

  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  if (resource.type() != Json::objectValue)
  {
    OrthancPlugins::Configuration::LogError("Resources of interest for the DICOMweb WADO-RS Retrieve client "
                                            "must be provided as a JSON object");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }

  std::string study, series, instance;
  if (!GetStringValue(study, resource, STUDY) ||
      study.empty())
  {
    OrthancPlugins::Configuration::LogError("A non-empty \"" + STUDY + "\" field is mandatory for the "
                                            "DICOMweb WADO-RS Retrieve client");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }

  GetStringValue(series, resource, SERIES);
  GetStringValue(instance, resource, INSTANCE);

  if (series.empty() && 
      !instance.empty())
  {
    OrthancPlugins::Configuration::LogError("When specifying a \"" + INSTANCE + "\" field in a call to DICOMweb "
                                            "WADO-RS Retrieve client, the \"" + SERIES + "\" field is mandatory");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }

  std::string tmpUri = "studies/" + study;
  if (!series.empty())
  {
    tmpUri += "/series/" + series;
    if (!instance.empty())
    {
      tmpUri += "/instances/" + instance;
    }
  }

  std::string uri;
  OrthancPlugins::UriEncode(uri, tmpUri, getArguments);

  OrthancPlugins::MemoryBuffer answerBody(context);
  std::map<std::string, std::string> answerHeaders;
  OrthancPlugins::CallServer(answerBody, answerHeaders, server, OrthancPluginHttpMethod_Get, httpHeaders, uri, "");

  std::vector<std::string> contentType;
  for (std::map<std::string, std::string>::const_iterator 
         it = answerHeaders.begin(); it != answerHeaders.end(); ++it)
  {
    std::string s = Orthanc::Toolbox::StripSpaces(it->first);
    Orthanc::Toolbox::ToLowerCase(s);
    if (s == "content-type")
    {
      Orthanc::Toolbox::TokenizeString(contentType, it->second, ';');
      break;
    }
  }

  if (contentType.empty())
  {
    OrthancPlugins::Configuration::LogError("No Content-Type provided by the remote WADO-RS server");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  Orthanc::Toolbox::ToLowerCase(contentType[0]);
  if (Orthanc::Toolbox::StripSpaces(contentType[0]) != MULTIPART_RELATED)
  {
    OrthancPlugins::Configuration::LogError("The remote WADO-RS server answers with a \"" + contentType[0] +
                                            "\" Content-Type, but \"" + MULTIPART_RELATED + "\" is expected");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  std::string type, boundary;
  for (size_t i = 1; i < contentType.size(); i++)
  {
    std::vector<std::string> tokens;
    Orthanc::Toolbox::TokenizeString(tokens, contentType[i], '=');

    if (tokens.size() == 2)
    {
      std::string s = Orthanc::Toolbox::StripSpaces(tokens[0]);
      Orthanc::Toolbox::ToLowerCase(s);

      if (s == "type")
      {
        type = Orthanc::Toolbox::StripSpaces(tokens[1]);

        // This covers the case where the content-type is quoted,
        // which COULD be the case 
        // cf. https://tools.ietf.org/html/rfc7231#section-3.1.1.1
        size_t len = type.length();
        if (len >= 2 &&
            type[0] == '"' &&
            type[len - 1] == '"')
        {
          type = type.substr(1, len - 2);
        }
        
        Orthanc::Toolbox::ToLowerCase(type);
      }
      else if (s == "boundary")
      {
        boundary = Orthanc::Toolbox::StripSpaces(tokens[1]);
      }
    }
  }

  if (type != APPLICATION_DICOM)
  {
    OrthancPlugins::Configuration::LogError("The remote WADO-RS server answers with a \"" + type +
                                            "\" multipart Content-Type, but \"" + APPLICATION_DICOM + "\" is expected");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  if (boundary.empty())
  {
    OrthancPlugins::Configuration::LogError("The remote WADO-RS server does not provide a boundary for its multipart answer");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  std::vector<OrthancPlugins::MultipartItem> parts;
  OrthancPlugins::ParseMultipartBody(parts, context, 
                                     reinterpret_cast<const char*>(answerBody.GetData()),
                                     answerBody.GetSize(), boundary);

  OrthancPlugins::Configuration::LogInfo("The remote WADO-RS server has provided " +
                                         boost::lexical_cast<std::string>(parts.size()) + 
                                         " DICOM instances");

  for (size_t i = 0; i < parts.size(); i++)
  {
    if (parts[i].contentType_ != APPLICATION_DICOM)
    {
      OrthancPlugins::Configuration::LogError("The remote WADO-RS server has provided a non-DICOM file in its multipart answer");
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);      
    }

    OrthancPlugins::MemoryBuffer tmp(context);
    tmp.RestApiPost("/instances", parts[i].data_, parts[i].size_, false);

    Json::Value result;
    tmp.ToJson(result);

    if (result.type() != Json::objectValue ||
        !result.isMember("ID") ||
        result["ID"].type() != Json::stringValue)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);      
    }
    else
    {
      instances.insert(result["ID"].asString());
    }
  }
}



void RetrieveFromServer(OrthancPluginRestOutput* output,
                        const char* /*url*/,
                        const OrthancPluginHttpRequest* request)
{
  static const std::string RESOURCES("Resources");
  static const char* HTTP_HEADERS = "HttpHeaders";
  static const std::string GET_ARGUMENTS = "Arguments";

  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "POST");
    return;
  }

  Orthanc::WebServiceParameters server(OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]));

  Json::Value body;
  Json::Reader reader;
  if (!reader.parse(request->body, request->body + request->bodySize, body) ||
      body.type() != Json::objectValue ||
      !body.isMember(RESOURCES) ||
      body[RESOURCES].type() != Json::arrayValue)
  {
    OrthancPlugins::Configuration::LogError("A request to the DICOMweb WADO-RS Retrieve client must provide a JSON object "
                                            "with the field \"" + RESOURCES + "\" containing an array of resources");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }

  std::map<std::string, std::string> httpHeaders;
  OrthancPlugins::ParseAssociativeArray(httpHeaders, body, HTTP_HEADERS);

  std::map<std::string, std::string> getArguments;
  OrthancPlugins::ParseAssociativeArray(getArguments, body, GET_ARGUMENTS);


  std::set<std::string> instances;
  for (Json::Value::ArrayIndex i = 0; i < body[RESOURCES].size(); i++)
  {
    RetrieveFromServerInternal(instances, server, httpHeaders, getArguments, body[RESOURCES][i]);
  }

  Json::Value status = Json::objectValue;
  status["Instances"] = Json::arrayValue;
  
  for (std::set<std::string>::const_iterator
         it = instances.begin(); it != instances.end(); ++it)
  {
    status["Instances"].append(*it);
  }

  std::string s = status.toStyledString();
  OrthancPluginAnswerBuffer(context, output, s.c_str(), s.size(), "application/json");
}
