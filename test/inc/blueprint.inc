/* An outcome assertion accepts successful transactions and precise failures. */
string blueprint_outcome(mapping report)
{
    if (report["status"] != "failed") return report["status"];
    return report["errors"][0]["code"];
}
