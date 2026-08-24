using System;
using System.IO;
using System.Text;
using Acecode.FeedbackUpload;

internal static class AcecodeFeedbackUploadHandlerTests
{
    private static int failures;

    private static void Check(bool condition, string description)
    {
        if (condition)
        {
            Console.WriteLine("PASS: " + description);
            return;
        }

        ++failures;
        Console.Error.WriteLine("FAIL: " + description);
    }

    private static void CheckFilename(
        string uploadName,
        string formName,
        bool expected,
        string description)
    {
        string safe;
        string error;
        bool actual = FeedbackUploadPolicy.TryValidateFilename(
            uploadName, formName, out safe, out error);
        Check(actual == expected, description + (actual ? string.Empty : " (" + error + ")"));
    }

    public static int Main()
    {
        string tempRoot = Path.Combine(
            Path.GetTempPath(), "acecode-feedback-policy-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(tempRoot);

            CheckFilename(
                "acecode-feedback-desktop-20260824-010203-windows-x64.zip",
                "acecode-feedback-desktop-20260824-010203-windows-x64.zip",
                true,
                "current ACECode package filename is accepted");
            CheckFilename(
                "..\\acecode-feedback-bad.zip",
                "..\\acecode-feedback-bad.zip",
                false,
                "path components are rejected");
            CheckFilename(
                "feedback.zip",
                "feedback.zip",
                false,
                "unexpected prefix is rejected");
            CheckFilename(
                "acecode-feedback-good.zip",
                "acecode-feedback-other.zip",
                false,
                "mismatched multipart names are rejected");

            Check(
                FeedbackUploadPolicy.IsEndpointPath("/aupdate/", "aupdate"),
                "configured endpoint normalizes to one trailing slash");
            Check(
                !FeedbackUploadPolicy.IsEndpointPath("/aupdate/child", "/aupdate/"),
                "child POST path is rejected");
            Check(
                FeedbackUploadPolicy.IsExpectedUserAgent("acecode-feedback"),
                "ACECode feedback user agent is accepted");
            Check(
                !FeedbackUploadPolicy.IsExpectedUserAgent("browser"),
                "unexpected user agent is rejected");

            Check(
                FeedbackUploadPolicy.HasZipSignature(
                    new byte[] { 0x50, 0x4b, 0x03, 0x04 }),
                "normal ZIP signature is accepted");
            Check(
                FeedbackUploadPolicy.HasZipSignature(
                    new byte[] { 0x50, 0x4b, 0x05, 0x06 }),
                "empty ZIP signature is accepted");
            Check(
                !FeedbackUploadPolicy.HasZipSignature(
                    new byte[] { 0x7b, 0x22, 0x78, 0x22 }),
                "non-ZIP signature is rejected");

            string siteRoot = Path.Combine(tempRoot, "site");
            string feedbackInside = Path.Combine(siteRoot, "feedback");
            string feedbackOutside = Path.Combine(tempRoot, "feedback");
            Check(
                FeedbackUploadPolicy.IsPathWithinDirectory(feedbackInside, siteRoot),
                "child storage is recognized as inside the web root");
            Check(
                !FeedbackUploadPolicy.IsPathWithinDirectory(feedbackOutside, siteRoot),
                "sibling storage is recognized as outside the web root");

            string storage = Path.Combine(tempRoot, "storage");
            Directory.CreateDirectory(storage);
            string filename = "acecode-feedback-collision.zip";
            string existing = Path.Combine(storage, filename);
            File.WriteAllText(existing, "existing", Encoding.ASCII);
            string temporary = Path.Combine(storage, ".uploading-test.tmp");
            File.WriteAllText(temporary, "new", Encoding.ASCII);
            string committed = FeedbackUploadStorage.CommitTemporaryFile(
                temporary, storage, filename);
            Check(committed != filename, "filename collision creates a derived name");
            Check(File.ReadAllText(existing, Encoding.ASCII) == "existing",
                "filename collision preserves the existing package");
            Check(File.ReadAllText(Path.Combine(storage, committed), Encoding.ASCII) == "new",
                "filename collision commits the new package");
            Check(!File.Exists(temporary), "atomic commit consumes the temporary file");
        }
        catch (Exception exception)
        {
            ++failures;
            Console.Error.WriteLine("FAIL: unhandled test exception: " + exception);
        }
        finally
        {
            try
            {
                if (Directory.Exists(tempRoot))
                {
                    Directory.Delete(tempRoot, true);
                }
            }
            catch (Exception exception)
            {
                ++failures;
                Console.Error.WriteLine("FAIL: test cleanup: " + exception.Message);
            }
        }

        Console.WriteLine("RESULT: " + (failures == 0 ? "PASS" : "FAIL"));
        return failures == 0 ? 0 : 1;
    }
}
