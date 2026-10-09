// CDDA Multiplayer launcher: shows the installed and the newest build of the
// fork, downloads and installs a new build, starts the game.
//
// Builds are GitHub releases of the fork's repository (the Windows workflow
// publishes one per build) with the game in cdda-mp-windows-x64.zip.
// The game lives in the "game" folder next to the launcher; on an update
// everything there is replaced except the player's own folders (saves,
// settings, templates...).
//
// Plain C# 5 for the csc.exe that comes with .NET Framework 4.x on every
// Windows: no extra runtime to install.

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

[assembly: System.Reflection.AssemblyTitle( "CDDA Multiplayer launcher" )]
[assembly: System.Reflection.AssemblyVersion( "1.0.0.0" )]

namespace CddaMpLauncher
{

internal class Release
{
    public string Tag = "";
    public string Name = "";
    public string Notes = "";
    public string ZipUrl = "";
    public long ZipSize;
}

internal static class GitHub
{
    public const string Repo = "zombo658/cataclysm-dda-mp";
    public const string Asset = "cdda-mp-windows-x64.zip";

    public static WebClient Client()
    {
        WebClient web = new WebClient();
        web.Headers.Add( "User-Agent", "cdda-mp-launcher" );
        web.Encoding = Encoding.UTF8;
        return web;
    }

    // The newest release; null with the reason in `error` if there is none.
    public static Release Latest( out string error )
    {
        error = null;
        string json;
        try {
            using( WebClient web = Client() ) {
                web.Headers.Add( "Accept", "application/vnd.github+json" );
                json = web.DownloadString( "https://api.github.com/repos/" + Repo + "/releases/latest" );
            }
        } catch( WebException e ) {
            error = e.Message;
            return null;
        }
        Release r = new Release();
        r.Tag = Field( json, "tag_name" );
        r.Name = Field( json, "name" );
        r.Notes = Field( json, "body" );
        // The asset: its name, then its download link and size.
        int at = json.IndexOf( "\"name\":\"" + Asset + "\"", StringComparison.Ordinal );
        if( at < 0 ) {
            at = json.IndexOf( "\"name\": \"" + Asset + "\"", StringComparison.Ordinal );
        }
        if( at < 0 ) {
            error = "the release " + r.Tag + " has no " + Asset;
            return null;
        }
        string rest = json.Substring( at );
        r.ZipUrl = Field( rest, "browser_download_url" );
        long.TryParse( Number( rest, "size" ), out r.ZipSize );
        return r;
    }

    // A string member of a JSON object, unescaped (enough for GitHub's answers).
    private static string Field( string json, string name )
    {
        Match m = Regex.Match( json, "\"" + name + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"" );
        if( !m.Success ) {
            return "";
        }
        return Regex.Unescape( m.Groups[1].Value );
    }

    private static string Number( string json, string name )
    {
        Match m = Regex.Match( json, "\"" + name + "\"\\s*:\\s*(\\d+)" );
        return m.Success ? m.Groups[1].Value : "0";
    }
}

internal class MainForm : Form
{
    // The player's own things in the game folder, kept over updates.
    private static readonly string[] Keep = {
        "save", "config", "templates", "memorial", "graveyard", "mods", "sound_user",
        "gfx_user", "font_user", "achievements", "screenshots", "debug.log"
    };

    private readonly string root;
    private readonly string gameDir;
    private readonly string versionFile;

    private readonly Label installedLabel = new Label();
    private readonly Label latestLabel = new Label();
    private readonly TextBox notes = new TextBox();
    private readonly ProgressBar progress = new ProgressBar();
    private readonly Label status = new Label();
    private readonly Button updateButton = new Button();
    private readonly Button playButton = new Button();
    private readonly Button checkButton = new Button();
    private readonly Button folderButton = new Button();

    private Release latest;
    private bool busy;

    public MainForm()
    {
        root = AppDomain.CurrentDomain.BaseDirectory;
        gameDir = Path.Combine( root, "game" );
        versionFile = Path.Combine( gameDir, "mp-build.txt" );

        Text = "CDDA Multiplayer";
        ClientSize = new Size( 640, 420 );
        MinimumSize = new Size( 480, 320 );
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font( "Segoe UI", 10f );

        installedLabel.SetBounds( 12, 12, 616, 22 );
        installedLabel.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        latestLabel.SetBounds( 12, 36, 616, 22 );
        latestLabel.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

        notes.Multiline = true;
        notes.ReadOnly = true;
        notes.ScrollBars = ScrollBars.Vertical;
        notes.SetBounds( 12, 64, 616, 236 );
        notes.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;

        progress.SetBounds( 12, 308, 616, 18 );
        progress.Anchor = AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        status.SetBounds( 12, 330, 616, 22 );
        status.Anchor = AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;

        playButton.Text = "Play";
        playButton.SetBounds( 508, 366, 120, 40 );
        playButton.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
        playButton.Click += delegate {
            Play();
        };
        updateButton.Text = "Update";
        updateButton.SetBounds( 380, 366, 120, 40 );
        updateButton.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
        updateButton.Click += delegate {
            DoUpdate();
        };
        checkButton.Text = "Check again";
        checkButton.SetBounds( 12, 366, 120, 40 );
        checkButton.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
        checkButton.Click += delegate {
            Check();
        };
        folderButton.Text = "Game folder";
        folderButton.SetBounds( 140, 366, 120, 40 );
        folderButton.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
        folderButton.Click += delegate {
            Directory.CreateDirectory( gameDir );
            Process.Start( "explorer.exe", "\"" + gameDir + "\"" );
        };

        Controls.AddRange( new Control[] {
            installedLabel, latestLabel, notes, progress, status,
            playButton, updateButton, checkButton, folderButton
        } );
        AcceptButton = playButton;
        Shown += delegate {
            Check();
        };
    }

    private string Installed()
    {
        try {
            return File.Exists( versionFile ) ? File.ReadAllText( versionFile ).Trim() : "";
        } catch( IOException ) {
            return "";
        }
    }

    private string GameExe()
    {
        return Path.Combine( gameDir, "cataclysm-tiles.exe" );
    }

    private void Refresh2()
    {
        string installed = Installed();
        bool hasGame = File.Exists( GameExe() );
        installedLabel.Text = "Installed: " + ( hasGame ? ( installed.Length > 0 ? installed : "unknown build" ) :
                                                "nothing yet" );
        if( latest != null ) {
            latestLabel.Text = "Newest: " + latest.Tag + ( latest.Tag == installed ? "  (you have it)" : "" );
        }
        playButton.Enabled = !busy && hasGame;
        updateButton.Enabled = !busy && latest != null && latest.Tag != installed;
        updateButton.Text = hasGame ? "Update" : "Install";
        checkButton.Enabled = !busy;
    }

    private void Check()
    {
        busy = true;
        status.Text = "Checking for a new build...";
        latestLabel.Text = "Newest: ...";
        Refresh2();
        ThreadPool.QueueUserWorkItem( delegate {
            string error;
            Release r = GitHub.Latest( out error );
            BeginInvoke( ( MethodInvoker )delegate {
                busy = false;
                latest = r;
                if( r == null ) {
                    latestLabel.Text = "Newest: unknown";
                    status.Text = "Can't check for a new build: " + error;
                } else {
                    notes.Text = ( r.Name.Length > 0 ? r.Name + "\r\n\r\n" : "" ) +
                                 r.Notes.Replace( "\r\n", "\n" ).Replace( "\n", "\r\n" );
                    status.Text = r.Tag == Installed() ? "You have the newest build." : "A new build is available.";
                }
                Refresh2();
            } );
        } );
    }

    private void DoUpdate()
    {
        if( latest == null ) {
            return;
        }
        if( Process.GetProcessesByName( "cataclysm-tiles" ).Length > 0 ) {
            MessageBox.Show( this, "Close the game first.", Text );
            return;
        }
        busy = true;
        Refresh2();
        Release r = latest;
        string zip = Path.Combine( root, GitHub.Asset + ".part" );
        WebClient web = GitHub.Client();
        web.DownloadProgressChanged += delegate( object s, DownloadProgressChangedEventArgs e ) {
            long total = e.TotalBytesToReceive > 0 ? e.TotalBytesToReceive : r.ZipSize;
            progress.Value = total > 0 ? ( int )Math.Min( 100, e.BytesReceived * 100 / total ) : 0;
            status.Text = string.Format( "Downloading {0}: {1} of {2} MB", r.Tag, e.BytesReceived >> 20,
                                         total >> 20 );
        };
        web.DownloadFileCompleted += delegate( object s, AsyncCompletedEventArgs e ) {
            web.Dispose();
            if( e.Error != null ) {
                Done( "Download failed: " + e.Error.Message );
                return;
            }
            status.Text = "Installing " + r.Tag + "...";
            progress.Style = ProgressBarStyle.Marquee;
            ThreadPool.QueueUserWorkItem( delegate {
                string error = Install( zip, r.Tag );
                BeginInvoke( ( MethodInvoker )delegate {
                    progress.Style = ProgressBarStyle.Blocks;
                    progress.Value = error == null ? 100 : 0;
                    Done( error == null ? "Installed " + r.Tag + "." : "Install failed: " + error );
                } );
            } );
        };
        status.Text = "Downloading " + r.Tag + "...";
        web.DownloadFileAsync( new Uri( r.ZipUrl ), zip );
    }

    private void Done( string message )
    {
        busy = false;
        status.Text = message;
        Refresh2();
    }

    // Unpacks the build next to the game, moves the player's folders into
    // it and puts it in the game's place. Null if done, else the error.
    private string Install( string zip, string tag )
    {
        string fresh = Path.Combine( root, "game.new" );
        string old = Path.Combine( root, "game.old" );
        try {
            if( Directory.Exists( fresh ) ) {
                Directory.Delete( fresh, true );
            }
            if( Directory.Exists( old ) ) {
                Directory.Delete( old, true );
            }
            ZipFile.ExtractToDirectory( zip, fresh );
            // Some zips have everything in one folder.
            string[] dirs = Directory.GetDirectories( fresh );
            if( Directory.GetFiles( fresh ).Length == 0 && dirs.Length == 1 ) {
                string inner = dirs[0];
                string moved = fresh + ".tmp";
                Directory.Move( inner, moved );
                Directory.Delete( fresh, true );
                Directory.Move( moved, fresh );
            }
            if( Directory.Exists( gameDir ) ) {
                foreach( string name in Keep ) {
                    string from = Path.Combine( gameDir, name );
                    string to = Path.Combine( fresh, name );
                    if( Directory.Exists( from ) ) {
                        if( Directory.Exists( to ) ) {
                            Directory.Delete( to, true );
                        }
                        Directory.Move( from, to );
                    } else if( File.Exists( from ) ) {
                        File.Copy( from, to, true );
                    }
                }
                Directory.Move( gameDir, old );
            }
            Directory.Move( fresh, gameDir );
            File.WriteAllText( versionFile, tag );
            File.Delete( zip );
            if( Directory.Exists( old ) ) {
                Directory.Delete( old, true );
            }
            return null;
        } catch( Exception e ) {
            return e.Message;
        }
    }

    private void Play()
    {
        try {
            ProcessStartInfo info = new ProcessStartInfo( GameExe() );
            info.WorkingDirectory = gameDir;
            Process.Start( info );
            Close();
        } catch( Exception e ) {
            MessageBox.Show( this, "Can't start the game: " + e.Message, Text );
        }
    }
}

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        // GitHub accepts only TLS 1.2+; old .NET defaults don't offer it.
        ServicePointManager.SecurityProtocol = ( SecurityProtocolType )3072;
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault( false );
        Application.Run( new MainForm() );
    }
}

} // namespace CddaMpLauncher
