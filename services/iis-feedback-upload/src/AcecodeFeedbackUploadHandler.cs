using System;
using System.Configuration;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Web;

namespace Acecode.FeedbackUpload
{
    public static class FeedbackUploadPolicy
    {
        public const long DefaultMaxFileBytes = 64L * 1024L * 1024L;
        public const long DefaultMinimumFreeBytes = 1024L * 1024L * 1024L;
        public const int MaximumFilenameLength = 220;

        private static readonly Regex FeedbackFilenamePattern = new Regex(
            @"\Aacecode-feedback-[A-Za-z0-9._-]+\.zip\z",
            RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);

        public static bool IsExpectedUserAgent(string userAgent)
        {
            return string.Equals(
                userAgent == null ? string.Empty : userAgent.Trim(),
                "acecode-feedback",
                StringComparison.OrdinalIgnoreCase);
        }

        public static string NormalizeEndpointPath(string configuredPath)
        {
            string value = string.IsNullOrWhiteSpace(configuredPath)
                ? "/aupdate/"
                : configuredPath.Trim();
            if (value.IndexOf('?') >= 0 || value.IndexOf('#') >= 0)
            {
                throw new ArgumentException("endpoint path must not contain a query or fragment");
            }

            value = value.Replace('\\', '/');
            if (!value.StartsWith("/", StringComparison.Ordinal))
            {
                value = "/" + value;
            }
            value = value.TrimEnd('/');
            return value.Length == 0 ? "/" : value + "/";
        }

        public static bool IsEndpointPath(string requestPath, string configuredPath)
        {
            if (string.IsNullOrWhiteSpace(requestPath))
            {
                return false;
            }

            string expected;
            try
            {
                expected = NormalizeEndpointPath(configuredPath);
            }
            catch (ArgumentException)
            {
                return false;
            }

            return string.Equals(
                requestPath.Replace('\\', '/'),
                expected,
                StringComparison.OrdinalIgnoreCase);
        }

        public static bool TryValidateFilename(
            string uploadedFilename,
            string formFilename,
            out string safeFilename,
            out string error)
        {
            safeFilename = string.Empty;
            error = string.Empty;
            if (string.IsNullOrWhiteSpace(uploadedFilename) ||
                string.IsNullOrWhiteSpace(formFilename))
            {
                error = "missing filename";
                return false;
            }

            string uploadValue = uploadedFilename.Trim();
            string formValue = formFilename.Trim();
            if (!string.Equals(uploadValue, formValue, StringComparison.Ordinal))
            {
                error = "multipart filenames do not match";
                return false;
            }
            if (uploadValue.Length > MaximumFilenameLength)
            {
                error = "filename is too long";
                return false;
            }

            try
            {
                if (!string.Equals(
                        Path.GetFileName(uploadValue),
                        uploadValue,
                        StringComparison.Ordinal))
                {
                    error = "filename contains a path component";
                    return false;
                }
            }
            catch (ArgumentException)
            {
                error = "filename is invalid";
                return false;
            }

            if (!FeedbackFilenamePattern.IsMatch(uploadValue))
            {
                error = "filename must match acecode-feedback-*.zip";
                return false;
            }

            safeFilename = uploadValue;
            return true;
        }

        public static bool HasZipSignature(byte[] prefix)
        {
            if (prefix == null || prefix.Length < 4 || prefix[0] != 0x50 || prefix[1] != 0x4b)
            {
                return false;
            }

            return (prefix[2] == 0x03 && prefix[3] == 0x04) ||
                   (prefix[2] == 0x05 && prefix[3] == 0x06) ||
                   (prefix[2] == 0x07 && prefix[3] == 0x08);
        }

        public static bool HasZipSignature(string path)
        {
            byte[] prefix = new byte[4];
            using (FileStream stream = new FileStream(
                path, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                if (stream.Read(prefix, 0, prefix.Length) != prefix.Length)
                {
                    return false;
                }
            }
            return HasZipSignature(prefix);
        }

        public static bool IsPathWithinDirectory(string candidatePath, string directoryPath)
        {
            string candidate = TrimDirectorySeparators(Path.GetFullPath(candidatePath));
            string directory = TrimDirectorySeparators(Path.GetFullPath(directoryPath));
            if (string.Equals(candidate, directory, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }

            string prefix = directory + Path.DirectorySeparatorChar;
            return candidate.StartsWith(prefix, StringComparison.OrdinalIgnoreCase);
        }

        public static bool HasRequiredFreeSpace(
            string storageDirectory,
            long incomingBytes,
            long minimumFreeBytes)
        {
            if (incomingBytes < 0 || minimumFreeBytes < 0 ||
                incomingBytes > long.MaxValue - minimumFreeBytes)
            {
                return false;
            }

            string fullPath = Path.GetFullPath(storageDirectory);
            string root = Path.GetPathRoot(fullPath);
            if (string.IsNullOrEmpty(root))
            {
                return false;
            }

            DriveInfo drive = new DriveInfo(root);
            return drive.IsReady &&
                   drive.AvailableFreeSpace >= incomingBytes + minimumFreeBytes;
        }

        public static string BuildCollisionFilename(
            string safeFilename,
            DateTime utcNow,
            Guid collisionId)
        {
            string basename = Path.GetFileNameWithoutExtension(safeFilename);
            string suffix = "-" + utcNow.ToUniversalTime().ToString(
                "yyyyMMdd-HHmmssfff", CultureInfo.InvariantCulture) +
                "-" + collisionId.ToString("N").Substring(0, 8);
            int maximumBaseLength = 248 - suffix.Length - 4;
            if (basename.Length > maximumBaseLength)
            {
                basename = basename.Substring(0, maximumBaseLength);
            }
            return basename + suffix + ".zip";
        }

        private static string TrimDirectorySeparators(string value)
        {
            return value.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        }
    }

    public static class FeedbackUploadStorage
    {
        public static string CommitTemporaryFile(
            string temporaryPath,
            string storageDirectory,
            string safeFilename)
        {
            string candidateName = safeFilename;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                string candidatePath = Path.Combine(storageDirectory, candidateName);
                try
                {
                    File.Move(temporaryPath, candidatePath);
                    return candidateName;
                }
                catch (IOException)
                {
                    if (!File.Exists(candidatePath))
                    {
                        throw;
                    }
                    candidateName = FeedbackUploadPolicy.BuildCollisionFilename(
                        safeFilename, DateTime.UtcNow, Guid.NewGuid());
                }
            }

            throw new IOException("could not allocate a unique feedback filename");
        }
    }

    public sealed class FeedbackUploadHandler : IHttpHandler
    {
        public bool IsReusable
        {
            get { return true; }
        }

        public void ProcessRequest(HttpContext context)
        {
            if (context == null)
            {
                throw new ArgumentNullException("context");
            }

            HttpRequest request = context.Request;
            if (!string.Equals(request.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase))
            {
                WriteFailure(context, 405, "method_not_allowed");
                return;
            }

            string endpointPath = ReadSetting("AcecodeFeedback.EndpointPath", "/aupdate/");
            if (!FeedbackUploadPolicy.IsEndpointPath(request.Path, endpointPath))
            {
                WriteFailure(context, 404, "upload_endpoint_not_found");
                return;
            }
            if (!FeedbackUploadPolicy.IsExpectedUserAgent(request.UserAgent))
            {
                WriteFailure(context, 403, "unexpected_client");
                return;
            }
            if (string.IsNullOrEmpty(request.ContentType) ||
                !request.ContentType.StartsWith(
                    "multipart/form-data", StringComparison.OrdinalIgnoreCase))
            {
                WriteFailure(context, 415, "multipart_form_data_required");
                return;
            }

            try
            {
                ProcessMultipartRequest(context);
            }
            catch (HttpRequestValidationException)
            {
                WriteFailure(context, 400, "invalid_form_data");
            }
            catch (Exception exception)
            {
                Trace.TraceError("ACECode feedback upload failed: {0}", exception);
                WriteFailure(context, 500, "storage_error");
            }
        }

        private static void ProcessMultipartRequest(HttpContext context)
        {
            HttpRequest request = context.Request;
            if (request.Files.Count != 1)
            {
                WriteFailure(context, 400, "exactly_one_file_required");
                return;
            }

            HttpPostedFile uploadedFile = request.Files["file"];
            if (uploadedFile == null)
            {
                WriteFailure(context, 400, "file_field_required");
                return;
            }

            string safeFilename;
            string filenameError;
            if (!FeedbackUploadPolicy.TryValidateFilename(
                    uploadedFile.FileName,
                    request.Form["filename"],
                    out safeFilename,
                    out filenameError))
            {
                WriteFailure(context, 400, filenameError);
                return;
            }

            long maxFileBytes = ReadLongSetting(
                "AcecodeFeedback.MaxFileBytes",
                FeedbackUploadPolicy.DefaultMaxFileBytes);
            if (uploadedFile.ContentLength <= 0 || uploadedFile.ContentLength > maxFileBytes)
            {
                WriteFailure(context, 413, "feedback_package_too_large");
                return;
            }

            string storageDirectory = ReadSetting("AcecodeFeedback.StoragePath", string.Empty);
            if (string.IsNullOrWhiteSpace(storageDirectory) ||
                !Path.IsPathRooted(storageDirectory))
            {
                WriteFailure(context, 500, "feedback_storage_is_not_configured");
                return;
            }

            storageDirectory = Path.GetFullPath(storageDirectory);
            string webRoot = context.Server.MapPath("~/");
            if (!string.IsNullOrEmpty(webRoot) &&
                FeedbackUploadPolicy.IsPathWithinDirectory(storageDirectory, webRoot))
            {
                WriteFailure(context, 500, "feedback_storage_must_be_outside_web_root");
                return;
            }

            Directory.CreateDirectory(storageDirectory);
            long minimumFreeBytes = ReadLongSetting(
                "AcecodeFeedback.MinimumFreeBytes",
                FeedbackUploadPolicy.DefaultMinimumFreeBytes);
            if (!FeedbackUploadPolicy.HasRequiredFreeSpace(
                    storageDirectory, uploadedFile.ContentLength, minimumFreeBytes))
            {
                WriteFailure(context, 507, "insufficient_feedback_storage");
                return;
            }

            string temporaryPath = Path.Combine(
                storageDirectory, ".uploading-" + Guid.NewGuid().ToString("N") + ".tmp");
            try
            {
                uploadedFile.SaveAs(temporaryPath);
                FileInfo savedFile = new FileInfo(temporaryPath);
                if (savedFile.Length != uploadedFile.ContentLength ||
                    savedFile.Length <= 0 || savedFile.Length > maxFileBytes)
                {
                    WriteFailure(context, 400, "stored_size_mismatch");
                    return;
                }
                if (!FeedbackUploadPolicy.HasZipSignature(temporaryPath))
                {
                    WriteFailure(context, 415, "zip_package_required");
                    return;
                }

                string storedFilename = FeedbackUploadStorage.CommitTemporaryFile(
                    temporaryPath, storageDirectory, safeFilename);
                temporaryPath = string.Empty;
                WriteSuccess(context, storedFilename, savedFile.Length);
            }
            finally
            {
                if (!string.IsNullOrEmpty(temporaryPath))
                {
                    try
                    {
                        if (File.Exists(temporaryPath))
                        {
                            File.Delete(temporaryPath);
                        }
                    }
                    catch (Exception cleanupException)
                    {
                        Trace.TraceError(
                            "ACECode feedback temporary-file cleanup failed: {0}",
                            cleanupException);
                    }
                }
            }
        }

        private static string ReadSetting(string key, string fallback)
        {
            string value = ConfigurationManager.AppSettings[key];
            return string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();
        }

        private static long ReadLongSetting(string key, long fallback)
        {
            string value = ConfigurationManager.AppSettings[key];
            long parsed;
            if (string.IsNullOrWhiteSpace(value) ||
                !long.TryParse(
                    value.Trim(),
                    NumberStyles.None,
                    CultureInfo.InvariantCulture,
                    out parsed) ||
                parsed < 0)
            {
                return fallback;
            }
            return parsed;
        }

        private static void WriteSuccess(
            HttpContext context,
            string storedFilename,
            long size)
        {
            PrepareJsonResponse(context, 201);
            context.Response.Write(
                "{\"success\":true,\"filename\":" + JsonString(storedFilename) +
                ",\"size\":" + size.ToString(CultureInfo.InvariantCulture) + "}");
        }

        private static void WriteFailure(HttpContext context, int statusCode, string error)
        {
            PrepareJsonResponse(context, statusCode);
            context.Response.Write(
                "{\"success\":false,\"error\":" + JsonString(error) + "}");
        }

        private static void PrepareJsonResponse(HttpContext context, int statusCode)
        {
            context.Response.Clear();
            context.Response.StatusCode = statusCode;
            context.Response.ContentType = "application/json";
            context.Response.ContentEncoding = Encoding.UTF8;
            context.Response.TrySkipIisCustomErrors = true;
            context.Response.Cache.SetCacheability(HttpCacheability.NoCache);
        }

        private static string JsonString(string value)
        {
            return HttpUtility.JavaScriptStringEncode(value ?? string.Empty, true);
        }
    }
}
