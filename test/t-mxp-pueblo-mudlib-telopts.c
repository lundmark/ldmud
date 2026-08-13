#pragma save_types, rtt_checks

#include "/inc/base.inc"
#include "/inc/client.inc"
#include "/inc/deep_eq.inc"

#include "/sys/input_to.h"
#include "/sys/telnet.h"

/* This test covers MXP/Pueblo support when H_NOECHO makes telnet
 * negotiation mudlib-owned. The driver must not negotiate TELOPT_MXP,
 * but Pueblo response detection remains driver-assisted.
 */

int server_done;
int client_done;
int test_failed;
string html_expected = "</xch_mudtext><img xch_mode=html>";
object client;

void check_done();

void record_failure()
{
    test_failed = 1;
}

void set_client(object ob)
{
    client = ob;
}

object get_client()
{
    return client;
}

void fail(string text)
{
    msg("FAILURE: %s\n", text);
    __MASTER_OBJECT__->record_failure();
    shutdown(1);
}

bytes b(string s)
{
    return to_bytes(s, "ISO-8859-1");
}

void noecho(int flag, object ob, int noecho)
{
}

void server_success()
{
    server_done = 1;
    check_done();
}

void client_success()
{
    client_done = 1;
    check_done();
}

void check_done()
{
    if (!test_failed && server_done && client_done)
    {
        msg("Success.\n");
        shutdown(0);
    }
}

void timeout()
{
    msg("FAILURE: Timed out, server_done=%d, client_done=%d.\n",
        server_done, client_done);
    __MASTER_OBJECT__->record_failure();
    shutdown(1);
}

void receive_server_command(string str)
{
    int state;
    int expected;

    if (str != "look")
    {
        msg("FAILURE: Server received %O instead of %O.\n", str, "look");
        __MASTER_OBJECT__->record_failure();
        shutdown(1);
    }

    state = interactive_info(this_object(), IC_MXP);
    expected = MXP_TELOPT | MXP_PUEBLO | MXP_PUEBLO_ACTIVE;
    if (state != expected)
    {
        msg("FAILURE: MXP state is 0x%x instead of 0x%x.\n", state, expected);
        __MASTER_OBJECT__->record_failure();
        shutdown(1);
    }

    write("READY\n");
    __MASTER_OBJECT__->server_success();
}

void receive_client_line(string str)
{
    int *received;
    int *expected;

    received = to_array(b(str));
    expected = to_array(b(html_expected + "READY"));

    if (!deep_eq(received, expected))
    {
        msg("FAILURE: Client received %O instead of %O.\n",
            received, expected);
        __MASTER_OBJECT__->record_failure();
        shutdown(1);
    }

    __MASTER_OBJECT__->client_success();
}

void send_client_protocol()
{
    binary_message(b("PUEBLOCLIENT 2.50\r\nlook\r\n"));
}

void enable_telnet()
{
    object cl;

    configure_interactive(this_object(), IC_TELNET_ENABLED, 1);

    cl = __MASTER_OBJECT__->get_client();
    if (!cl)
        fail("Client object was not registered.");
    cl->send_client_protocol();
}

void run_server()
{
    configure_interactive(this_object(), IC_MXP, MXP_TELOPT | MXP_PUEBLO);
    if (interactive_info(this_object(), IC_MXP) != (MXP_TELOPT | MXP_PUEBLO))
        fail("Initial MXP request state is wrong.");

    input_to("receive_server_command");
    call_out("enable_telnet", 0);
    call_out("timeout", 2 * __ALARM_TIME__);
}

void run_client()
{
    __MASTER_OBJECT__->set_client(this_object());
    input_to("receive_client_line");
    call_out("timeout", 2 * __ALARM_TIME__);
}

void run_test()
{
    msg("\nRunning test for MXP/Pueblo with mudlib telopts:\n"
          "------------------------------------------------\n");

    connect_self("run_server", "run_client");
}

string *epilog(int eflag)
{
    set_driver_hook(H_NOECHO, "noecho");
    run_test();
    return 0;
}
