#pragma save_types, rtt_checks

#include "/inc/base.inc"
#include "/inc/client.inc"

/* This test covers the Pueblo response being limited to the connection's
 * initial line. A later PUEBLOCLIENT command must be delivered normally.
 */

object client;

void fail(string text)
{
    msg("FAILURE: %s\n", text);
    shutdown(1);
}

bytes b(string s)
{
    return to_bytes(s, "ISO-8859-1");
}

void set_client(object ob)
{
    client = ob;
}

object get_client()
{
    return client;
}

void timeout()
{
    msg("FAILURE: Timed out.\n");
    shutdown(1);
}

void receive_second_command(string str)
{
    int state;

    if (str != "PUEBLOCLIENT 2.50")
    {
        msg("FAILURE: Server received %O instead of %O.\n",
            str, "PUEBLOCLIENT 2.50");
        shutdown(1);
    }

    state = interactive_info(this_object(), IC_MXP);
    if (state != MXP_PUEBLO)
    {
        msg("FAILURE: MXP state is 0x%x instead of 0x%x.\n",
            state, MXP_PUEBLO);
        shutdown(1);
    }

    write("READY\n");
}

void receive_first_command(string str)
{
    if (str != "hello")
    {
        msg("FAILURE: Server received %O instead of %O.\n", str, "hello");
        shutdown(1);
    }

    configure_interactive(this_object(), IC_MXP, MXP_PUEBLO);
    input_to("receive_second_command");
}

void receive_client_line(string str)
{
    if (str != "READY")
    {
        msg("FAILURE: Client received %O instead of %O.\n", str, "READY");
        shutdown(1);
    }

    msg("Success.\n");
    shutdown(0);
}

void send_client_protocol()
{
    binary_message(b("hello\r\nPUEBLOCLIENT 2.50\r\n"));
}

void run_server()
{
    object cl;

    input_to("receive_first_command");

    cl = __MASTER_OBJECT__->get_client();
    if (!cl)
        fail("Client object was not registered.");
    cl->send_client_protocol();
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
    msg("\nRunning test for Pueblo initial-line detection:\n"
          "-----------------------------------------------\n");

    connect_self("run_server", "run_client");
}

string *epilog(int eflag)
{
    run_test();
    return 0;
}
