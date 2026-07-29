#include <r_client.h>
#include <r_client_runtime.h>
#include <r_client_workflow.h>

int main(void) {
    r_request_policy_t policy;
    r_client_default_request_policy(&policy);
    return policy.unit_ms == 0;
}
