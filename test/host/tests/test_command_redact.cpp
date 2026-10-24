/*
 * redact_secrets(): passwords typed in local commands are never echoed or kept in history.
 */

#include "command_redact.hpp"
#include "test.hpp"

static std::string redact(const std::string& cmd, bool expect_secret)
{
    bool has_secret = !expect_secret;
    std::string out = redact_secrets(cmd, &has_secret);
    CHECK_EQ(has_secret, expect_secret);
    return out;
}

TEST(redact_connect_password)
{
    CHECK_EQ(redact("connect HomeNet hunter2", true), std::string("connect HomeNet ****"));
}

TEST(redact_connect_quoted_ssid)
{
    CHECK_EQ(redact("connect \"My Home Net\" hunter2", true), std::string("connect \"My Home Net\" ****"));
}

TEST(redact_connect_without_password_is_unchanged)
{
    CHECK_EQ(redact("connect HomeNet", false), std::string("connect HomeNet"));
}

TEST(redact_ssh_password)
{
    CHECK_EQ(redact("ssh 10.0.0.2 22 pi s3cret", true), std::string("ssh 10.0.0.2 22 pi ****"));
}

TEST(redact_ssh_password_with_spaces_hides_the_rest)
{
    std::string out = redact("ssh host 22 user pass word more", true);
    CHECK_EQ(out, std::string("ssh host 22 user ****"));
    CHECK(out.find("word") == std::string::npos);
}

TEST(redact_ssh_extra_spaces)
{
    CHECK_EQ(redact("ssh  host   22 user    s3cret", true), std::string("ssh  host   22 user    ****"));
}

TEST(redact_ssh_without_password_is_unchanged)
{
    CHECK_EQ(redact("ssh host 22 user", false), std::string("ssh host 22 user"));
}

TEST(redact_sshkey_passphrase)
{
    CHECK_EQ(redact("sshkey host 22 pi id.pem phrase", true), std::string("sshkey host 22 pi id.pem ****"));
}

TEST(redact_sshkey_without_passphrase_is_unchanged)
{
    CHECK_EQ(redact("sshkey host 22 pi id.pem", false), std::string("sshkey host 22 pi id.pem"));
}

TEST(redact_other_commands_untouched)
{
    CHECK_EQ(redact("help", false), std::string("help"));
    CHECK_EQ(redact("connectx a b", false), std::string("connectx a b"));
    CHECK_EQ(redact("sshx a b c d", false), std::string("sshx a b c d"));
    CHECK_EQ(redact("ls -la /tmp", false), std::string("ls -la /tmp"));
    CHECK_EQ(redact("", false), std::string(""));
}

TEST(split_args_quotes_group_and_are_dropped)
{
    std::vector<std::string> a = split_command_args("ssh host 22 user \"my pass word\"");
    CHECK_EQ(a.size(), (size_t)5);
    CHECK_EQ(a[4], std::string("my pass word"));
}

TEST(split_args_repeated_spaces_add_no_empty_arguments)
{
    std::vector<std::string> a = split_command_args("ssh  host   22 user pw ");
    CHECK_EQ(a.size(), (size_t)5);
    CHECK_EQ(a[1], std::string("host"));
    CHECK_EQ(a[2], std::string("22"));
}

TEST(split_args_quoted_empty_is_an_argument)
{
    std::vector<std::string> a = split_command_args("ssh host 22 user \"\"");
    CHECK_EQ(a.size(), (size_t)5);
    CHECK_EQ(a[4], std::string());
}

TEST(split_args_agree_with_redaction)
{
    // The argument redact_secrets() hides is the one the ssh command takes as the password
    bool has_secret = false;
    CHECK_EQ(redact_secrets("ssh host 22 user \"my pass\"", &has_secret), std::string("ssh host 22 user ****"));
    CHECK(has_secret);
}
