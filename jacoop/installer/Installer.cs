// JACoop update installer - single-file GUI installer.
// Build: csc /target:winexe Installer.cs /res:<payload>,<name> ...
// The four build files are embedded as manifest resources.

using System;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Windows.Forms;

internal static class Installer
{
    private const string DefaultDir = @"C:\Games\JACoop";

    // resource name -> subfolder under the install dir ("" = root)
    private static readonly string[,] Targets =
    {
        { "openjk_sp.x86_64.exe",    ""     },
        { "openjk_sp.x86_64.exe",    "bin"  },
        { "rdsp-vanilla_x86_64.dll", ""     },
        { "rdsp-vanilla_x86_64.dll", "bin"  },
        { "jagamex86_64.dll",        ""     },
        { "zz_jacoop.pk3",           "base" },
    };

    [STAThread]
    private static int Main(string[] args)
    {
        // silent mode for testing / power users:
        //   JACoop-Setup.exe /quiet "D:\Games\JACoop"
        if (args.Length >= 2 && args[0] == "/quiet")
        {
            TextWriter log = new StringWriter();
            bool ok = Install(args[1], log);
            string logPath = Path.Combine(
                Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location),
                "JACoop-Setup-log.txt");
            File.WriteAllText(logPath, log.ToString());
            return ok ? 0 : 1;
        }

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new InstallerForm());
        return 0;
    }

    internal static bool LooksLikeJACoop(string dir)
    {
        try
        {
            return File.Exists(Path.Combine(dir, "JACoop.exe"))
                || File.Exists(Path.Combine(dir, "jagamex86_64.dll"))
                || Directory.Exists(Path.Combine(dir, "base"))
                || Directory.Exists(Path.Combine(dir, "bin"));
        }
        catch { return false; }
    }

    // Returns true on success; progress/errors go to log.
    internal static bool Install(string dest, TextWriter log)
    {
        Assembly asm = Assembly.GetExecutingAssembly();
        try
        {
            Directory.CreateDirectory(Path.Combine(dest, "bin"));
            Directory.CreateDirectory(Path.Combine(dest, "base"));

            string bak = Path.Combine(dest, "backup-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            bool madeBackup = false;

            for (int i = 0; i < Targets.GetLength(0); i++)
            {
                string name = Targets[i, 0];
                string sub = Targets[i, 1];
                string dst = sub.Length == 0
                    ? Path.Combine(dest, name)
                    : Path.Combine(dest, sub, name);

                if (File.Exists(dst))
                {
                    if (!madeBackup)
                    {
                        Directory.CreateDirectory(bak);
                        madeBackup = true;
                    }
                    File.Copy(dst, Path.Combine(bak, name), true);
                }

                byte[] payload;
                using (Stream res = asm.GetManifestResourceStream(name))
                {
                    if (res == null)
                    {
                        log.WriteLine("ERROR: embedded file missing: " + name);
                        return false;
                    }
                    using (MemoryStream ms = new MemoryStream())
                    {
                        res.CopyTo(ms);
                        payload = ms.ToArray();
                    }
                }
                File.WriteAllBytes(dst, payload);

                // verify the bytes that landed on disk
                using (FileStream fs = File.OpenRead(dst))
                {
                    if (!Md5(fs).Equals(Md5(new MemoryStream(payload))))
                    {
                        log.WriteLine("ERROR: verify failed for " + dst);
                        return false;
                    }
                }
                log.WriteLine("installed  " + dst);
            }

            if (madeBackup)
                log.WriteLine("previous files backed up to " + bak);
            log.WriteLine("done.");
            return true;
        }
        catch (Exception ex)
        {
            log.WriteLine("ERROR: " + ex.Message);
            log.WriteLine("(if the game is running, close it and retry)");
            return false;
        }
    }

    private static string Md5(Stream s)
    {
        using (MD5 md5 = MD5.Create())
            return BitConverter.ToString(md5.ComputeHash(s));
    }

    private sealed class InstallerForm : Form
    {
        private readonly TextBox pathBox;
        private readonly TextBox logBox;
        private readonly Button installButton;
        private readonly Label statusLabel;

        internal InstallerForm()
        {
            Text = "JACoop update installer";
            StartPosition = FormStartPosition.CenterScreen;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            ClientSize = new Size(560, 320);

            Label l1 = new Label
            {
                Text = "JACoop folder (where JACoop.exe lives):",
                Location = new Point(12, 14),
                AutoSize = true,
            };
            pathBox = new TextBox
            {
                Text = DefaultDir,
                Location = new Point(12, 34),
                Width = 440,
            };
            Button browse = new Button
            {
                Text = "Browse...",
                Location = new Point(460, 32),
                Width = 88,
            };
            browse.Click += delegate { Browse(); };

            installButton = new Button
            {
                Text = "Install update",
                Location = new Point(12, 66),
                Width = 120,
            };
            installButton.Click += delegate { DoInstall(); };

            statusLabel = new Label
            {
                Location = new Point(140, 71),
                AutoSize = true,
                ForeColor = Color.DarkOrange,
            };

            logBox = new TextBox
            {
                Location = new Point(12, 100),
                Size = new Size(536, 208),
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Vertical,
                Font = new Font("Consolas", 8.5f),
            };

            Controls.Add(l1);
            Controls.Add(pathBox);
            Controls.Add(browse);
            Controls.Add(installButton);
            Controls.Add(statusLabel);
            Controls.Add(logBox);

            UpdateStatus();
            pathBox.TextChanged += delegate { UpdateStatus(); };
        }

        private void UpdateStatus()
        {
            string dir = pathBox.Text.Trim().Trim('"');
            if (dir.Length == 0)
            {
                statusLabel.Text = "";
            }
            else if (LooksLikeJACoop(dir))
            {
                statusLabel.ForeColor = Color.ForestGreen;
                statusLabel.Text = "looks like a JACoop install";
            }
            else
            {
                statusLabel.ForeColor = Color.DarkOrange;
                statusLabel.Text = "not recognised - check the path";
            }
        }

        private void Browse()
        {
            // OpenFileDialog hack -> modern folder picker (no extra deps)
            using (OpenFileDialog ofd = new OpenFileDialog())
            {
                ofd.ValidateNames = false;
                ofd.CheckFileExists = false;
                ofd.CheckPathExists = true;
                ofd.FileName = "Select this folder";
                ofd.Title = "Select your JACoop folder";
                ofd.Filter = "Folders|*.";
                if (ofd.ShowDialog(this) == DialogResult.OK)
                    pathBox.Text = Path.GetDirectoryName(ofd.FileName);
            }
        }

        private void DoInstall()
        {
            string dir = pathBox.Text.Trim().Trim('"');
            if (dir.Length == 0)
            {
                MessageBox.Show(this, "Pick a folder first.", "JACoop installer",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (!LooksLikeJACoop(dir))
            {
                DialogResult r = MessageBox.Show(this,
                    "\"" + dir + "\" does not look like a JACoop folder.\n\nInstall there anyway?",
                    "JACoop installer", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                if (r != DialogResult.Yes)
                    return;
            }

            installButton.Enabled = false;
            logBox.Clear();
            StringWriter log = new StringWriter();
            bool ok = Install(dir, log);
            logBox.Text = log.ToString();

            if (ok)
            {
                statusLabel.ForeColor = Color.ForestGreen;
                statusLabel.Text = "update installed";
                MessageBox.Show(this, "JACoop is up to date. Have fun!",
                    "JACoop installer", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            else
            {
                MessageBox.Show(this, "Install failed - see the log below.",
                    "JACoop installer", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            installButton.Enabled = true;
        }
    }
}
