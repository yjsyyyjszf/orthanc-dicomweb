/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2015 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
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


#include "Wado.h"
#include "Plugin.h"

#include "../Orthanc/Core/OrthancException.h"
#include "../Orthanc/Core/ImageFormats/PngReader.h"
#include "JpegWriter.h"
#include "Configuration.h"

#include <string>

static bool MapWadoToOrthancIdentifier(std::string& orthanc,
                                       char* (*func) (OrthancPluginContext*, const char*),
                                       const std::string& dicom)
{
  char* tmp = func(context_, dicom.c_str());

  if (tmp)
  {
    orthanc = tmp;
    OrthancPluginFreeString(context_, tmp);
    return true;
  }
  else
  {
    return false;
  }
}


static bool LocateInstance(std::string& instance,
                           std::string& contentType,
                           const OrthancPluginHttpRequest* request)
{
  std::string requestType, studyUid, seriesUid, objectUid;

  for (uint32_t i = 0; i < request->getCount; i++)
  {
    std::string key(request->getKeys[i]);
    std::string value(request->getValues[i]);

    if (key == "studyUID")
    {
      studyUid = value;
    }
    else if (key == "seriesUID")
    {
      seriesUid = value;
    }
    else if (key == "objectUID")  // In WADO, "objectUID" corresponds to "SOPInstanceUID"
    {
      objectUid = value;
    }
    else if (key == "requestType")
    {
      requestType = value;
    }
    else if (key == "contentType")
    {
      contentType = value;
    }
  }

  if (requestType != "WADO")
  {
    std::string msg = "WADO: Invalid requestType: \"" + requestType + "\"";
    OrthancPluginLogError(context_, msg.c_str());
    return false;
  }

  if (objectUid.empty())
  {
    OrthancPluginLogError(context_, "WADO: No SOPInstanceUID provided");
    return false;
  }

  if (!MapWadoToOrthancIdentifier(instance, OrthancPluginLookupInstance, objectUid))
  {
    std::string msg = "WADO: No such SOPInstanceUID in Orthanc: \"" + objectUid + "\"";
    OrthancPluginLogError(context_, msg.c_str());
    return false;
  }

  /**
   * Below are only sanity checks to ensure that the possibly provided
   * "seriesUID" and "studyUID" match that of the provided instance.
   **/

  if (!seriesUid.empty())
  {
    std::string series;
    if (!MapWadoToOrthancIdentifier(series, OrthancPluginLookupSeries, seriesUid))
    {
      std::string msg = "WADO: No such SeriesInstanceUID in Orthanc: \"" + seriesUid + "\"";
      OrthancPluginLogError(context_, msg.c_str());
      return false;
    }
    else
    {
      Json::Value info;
      if (!OrthancPlugins::RestApiGetJson(info, context_, "/instances/" + instance + "/series") ||
          info["MainDicomTags"]["SeriesInstanceUID"] != seriesUid)
      {
        std::string msg = "WADO: Instance " + objectUid + " does not belong to series " + seriesUid;
        OrthancPluginLogError(context_, msg.c_str());
        return false;
      }
    }
  }
  
  if (!studyUid.empty())
  {
    std::string study;
    if (!MapWadoToOrthancIdentifier(study, OrthancPluginLookupStudy, studyUid))
    {
      std::string msg = "WADO: No such StudyInstanceUID in Orthanc: \"" + studyUid + "\"";
      OrthancPluginLogError(context_, msg.c_str());
      return false;
    }
    else
    {
      Json::Value info;
      if (!OrthancPlugins::RestApiGetJson(info, context_, "/instances/" + instance + "/study") ||
          info["MainDicomTags"]["StudyInstanceUID"] != studyUid)
      {
        std::string msg = "WADO: Instance " + objectUid + " does not belong to study " + studyUid;
        OrthancPluginLogError(context_, msg.c_str());
        return false;
      }
    }
  }
  
  return true;
}


static int32_t AnswerDicom(OrthancPluginRestOutput* output,
                           const std::string& instance)
{
  std::string uri = "/instances/" + instance + "/file";

  std::string dicom;
  if (OrthancPlugins::RestApiGetString(dicom, context_, uri))
  {
    OrthancPluginAnswerBuffer(context_, output, dicom.c_str(), dicom.size(), "application/dicom");
    return 0;
  }
  else
  {
    std::string msg = "WADO: Unable to retrieve DICOM file from " + uri;
    OrthancPluginLogError(context_, msg.c_str());
    return -1;
  }
}


static bool RetrievePngPreview(std::string& png,
                               const std::string& instance)
{
  std::string uri = "/instances/" + instance + "/preview";

  if (OrthancPlugins::RestApiGetString(png, context_, uri))
  {
    return true;
  }
  else
  {
    std::string msg = "WADO: Unable to generate a preview image for " + uri;
    OrthancPluginLogError(context_, msg.c_str());
    return false;
  }
}


static int32_t AnswerPngPreview(OrthancPluginRestOutput* output,
                                const std::string& instance)
{
  std::string png;
  if (!RetrievePngPreview(png, instance))
  {
    return -1;
  }

  OrthancPluginAnswerBuffer(context_, output, png.c_str(), png.size(), "image/png");
  return 0;
}


static int32_t AnswerJpegPreview(OrthancPluginRestOutput* output,
                                 const std::string& instance)
{
  // Retrieve the preview in the PNG format
  std::string png;
  if (!RetrievePngPreview(png, instance))
  {
    return -1;
  }

  // Decode the PNG file
  Orthanc::PngReader reader;
  reader.ReadFromMemory(png);

  // Convert to JPEG
  OrthancPlugins::JpegWriter writer;
  std::string jpeg;
  writer.WriteToMemory(jpeg, reader);

  OrthancPluginAnswerBuffer(context_, output, jpeg.c_str(), jpeg.size(), "image/jpeg");
  return 0;
}


int32_t WadoCallback(OrthancPluginRestOutput* output,
                     const char* url,
                     const OrthancPluginHttpRequest* request)
{
  try
  {
    if (request->method != OrthancPluginHttpMethod_Get)
    {
      OrthancPluginSendMethodNotAllowed(context_, output, "GET");
      return -1;
    }

    std::string instance;
    std::string contentType = "image/jpg";  // By default, JPEG image will be returned
    if (!LocateInstance(instance, contentType, request))
    {
      return -1;
    }

    if (contentType == "application/dicom")
    {
      return AnswerDicom(output, instance);
    }
    else if (contentType == "image/png")
    {
      return AnswerPngPreview(output, instance);
    }
    else if (contentType == "image/jpeg" ||
             contentType == "image/jpg")
    {
      return AnswerJpegPreview(output, instance);
    }
    else
    {
      std::string msg = "WADO: Unsupported content type: \"" + contentType + "\"";
      OrthancPluginLogError(context_, msg.c_str());
      return -1;
    }

    return 0;
  }
  catch (Orthanc::OrthancException& e)
  {
    OrthancPluginLogError(context_, e.What());
    return -1;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}