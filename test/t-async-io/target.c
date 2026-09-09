void callback(int result)
{
    raise_error("Destroyed callback target executed.\n");
}

void submit()
{
    async_write("destroyed", "survives", 1, #'callback);
}
