/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	gssapi.h
 *	The C bindings of the Generic Security Service API (RFC 2744), types
 *	and constants only. The HTTP Negotiate scheme is written against
 *	these declarations and reaches every function through a library it
 *	opens by name when it runs; the system has none, so the scheme is
 *	never offered (Basic, Digest and NTLM are).
 */

#ifndef __TS_SHIM_GSSAPI_H__
#define __TS_SHIM_GSSAPI_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int OM_uint32;

/* An object identifier: a length and the DER-encoded bytes of the value. */
typedef struct gss_OID_desc_struct {
	OM_uint32 length;
	void     *elements;
} gss_OID_desc, *gss_OID;

typedef struct gss_OID_set_desc_struct {
	size_t   count;
	gss_OID  elements;
} gss_OID_set_desc, *gss_OID_set;

/* A counted byte string, used for every token and display string. */
typedef struct gss_buffer_desc_struct {
	size_t  length;
	void   *value;
} gss_buffer_desc, *gss_buffer_t;

typedef struct gss_channel_bindings_struct {
	OM_uint32       initiator_addrtype;
	gss_buffer_desc initiator_address;
	OM_uint32       acceptor_addrtype;
	gss_buffer_desc acceptor_address;
	gss_buffer_desc application_data;
} *gss_channel_bindings_t;

/* Opaque handles owned by the implementation. */
struct gss_name_struct;
typedef struct gss_name_struct *gss_name_t;
struct gss_cred_id_struct;
typedef struct gss_cred_id_struct *gss_cred_id_t;
struct gss_ctx_id_struct;
typedef struct gss_ctx_id_struct *gss_ctx_id_t;

typedef OM_uint32 gss_qop_t;

/* Null values for each handle type. */
#define GSS_C_NO_NAME              ((gss_name_t)0)
#define GSS_C_NO_BUFFER            ((gss_buffer_t)0)
#define GSS_C_NO_OID               ((gss_OID)0)
#define GSS_C_NO_OID_SET           ((gss_OID_set)0)
#define GSS_C_NO_CONTEXT           ((gss_ctx_id_t)0)
#define GSS_C_NO_CREDENTIAL        ((gss_cred_id_t)0)
#define GSS_C_NO_CHANNEL_BINDINGS  ((gss_channel_bindings_t)0)
#define GSS_C_EMPTY_BUFFER         {0, NULL}

/* Request flags for establishing a context. */
#define GSS_C_DELEG_FLAG         1
#define GSS_C_MUTUAL_FLAG        2
#define GSS_C_REPLAY_FLAG        4
#define GSS_C_SEQUENCE_FLAG      8
#define GSS_C_CONF_FLAG         16
#define GSS_C_INTEG_FLAG        32
#define GSS_C_ANON_FLAG         64
#define GSS_C_PROT_READY_FLAG  128
#define GSS_C_TRANS_FLAG       256
#define GSS_C_DELEG_POLICY_FLAG 32768

/* A lifetime of "as long as the credential allows". */
#define GSS_C_INDEFINITE ((OM_uint32)0xFFFFFFFFUL)

/* Which of the two status codes a display call is being asked about. */
#define GSS_C_GSS_CODE  1
#define GSS_C_MECH_CODE 2

/* A status code packs three independent fields. The masks and shifts below
 * are what the accessors take apart. */
#define GSS_C_CALLING_ERROR_OFFSET 24
#define GSS_C_ROUTINE_ERROR_OFFSET 16
#define GSS_C_SUPPLEMENTARY_OFFSET  0
#define GSS_C_CALLING_ERROR_MASK ((OM_uint32)0377U)
#define GSS_C_ROUTINE_ERROR_MASK ((OM_uint32)0377U)
#define GSS_C_SUPPLEMENTARY_MASK ((OM_uint32)0177777U)

#define GSS_CALLING_ERROR(x) \
	((x) & (GSS_C_CALLING_ERROR_MASK << GSS_C_CALLING_ERROR_OFFSET))
#define GSS_ROUTINE_ERROR(x) \
	((x) & (GSS_C_ROUTINE_ERROR_MASK << GSS_C_ROUTINE_ERROR_OFFSET))
#define GSS_SUPPLEMENTARY_INFO(x) \
	((x) & (GSS_C_SUPPLEMENTARY_MASK << GSS_C_SUPPLEMENTARY_OFFSET))
#define GSS_ERROR(x) \
	((x) & ((GSS_C_CALLING_ERROR_MASK << GSS_C_CALLING_ERROR_OFFSET) | \
	        (GSS_C_ROUTINE_ERROR_MASK << GSS_C_ROUTINE_ERROR_OFFSET)))

#define GSS_S_COMPLETE ((OM_uint32)0U)

/* Calling errors: the caller passed something wrong. */
#define GSS_S_CALL_INACCESSIBLE_READ  (((OM_uint32)1U) << GSS_C_CALLING_ERROR_OFFSET)
#define GSS_S_CALL_INACCESSIBLE_WRITE (((OM_uint32)2U) << GSS_C_CALLING_ERROR_OFFSET)
#define GSS_S_CALL_BAD_STRUCTURE      (((OM_uint32)3U) << GSS_C_CALLING_ERROR_OFFSET)

/* Routine errors: the call itself could not be carried out. */
#define GSS_S_BAD_MECH             (((OM_uint32)1U)  << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_BAD_NAME             (((OM_uint32)2U)  << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_BAD_NAMETYPE         (((OM_uint32)3U)  << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_BAD_BINDINGS         (((OM_uint32)4U)  << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_BAD_STATUS           (((OM_uint32)5U)  << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_BAD_SIG              (((OM_uint32)6U)  << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_BAD_MIC              GSS_S_BAD_SIG
#define GSS_S_NO_CRED              (((OM_uint32)7U)  << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_NO_CONTEXT           (((OM_uint32)8U)  << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_DEFECTIVE_TOKEN      (((OM_uint32)9U)  << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_DEFECTIVE_CREDENTIAL (((OM_uint32)10U) << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_CREDENTIALS_EXPIRED  (((OM_uint32)11U) << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_CONTEXT_EXPIRED      (((OM_uint32)12U) << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_FAILURE              (((OM_uint32)13U) << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_BAD_QOP              (((OM_uint32)14U) << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_UNAUTHORIZED         (((OM_uint32)15U) << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_UNAVAILABLE          (((OM_uint32)16U) << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_DUPLICATE_ELEMENT    (((OM_uint32)17U) << GSS_C_ROUTINE_ERROR_OFFSET)
#define GSS_S_NAME_NOT_MN          (((OM_uint32)18U) << GSS_C_ROUTINE_ERROR_OFFSET)

/* Supplementary information: bits that qualify an otherwise complete call. */
#define GSS_S_CONTINUE_NEEDED (((OM_uint32)1U) << (GSS_C_SUPPLEMENTARY_OFFSET + 0))
#define GSS_S_DUPLICATE_TOKEN (((OM_uint32)1U) << (GSS_C_SUPPLEMENTARY_OFFSET + 1))
#define GSS_S_OLD_TOKEN       (((OM_uint32)1U) << (GSS_C_SUPPLEMENTARY_OFFSET + 2))
#define GSS_S_UNSEQ_TOKEN     (((OM_uint32)1U) << (GSS_C_SUPPLEMENTARY_OFFSET + 3))
#define GSS_S_GAP_TOKEN       (((OM_uint32)1U) << (GSS_C_SUPPLEMENTARY_OFFSET + 4))

/* The standard name types, as object identifiers supplied by the
 * implementation. Nothing here defines them; the code that names them
 * carries its own copies of the encodings. */
extern gss_OID GSS_C_NT_USER_NAME;
extern gss_OID GSS_C_NT_MACHINE_UID_NAME;
extern gss_OID GSS_C_NT_STRING_UID_NAME;
extern gss_OID GSS_C_NT_HOSTBASED_SERVICE_X;
extern gss_OID GSS_C_NT_HOSTBASED_SERVICE;
extern gss_OID GSS_C_NT_ANONYMOUS;
extern gss_OID GSS_C_NT_EXPORT_NAME;

/* The calls the HTTP Negotiate scheme uses. It takes the type of each from
 * the declaration here and then looks the entry point up by name in the
 * library it opens, so these are never called through the names below and
 * need no definitions. */
OM_uint32 gss_import_name(OM_uint32 *minor_status,
                          const gss_buffer_t input_name_buffer,
                          const gss_OID input_name_type,
                          gss_name_t *output_name);
OM_uint32 gss_release_name(OM_uint32 *minor_status, gss_name_t *input_name);
OM_uint32 gss_release_buffer(OM_uint32 *minor_status, gss_buffer_t buffer);
OM_uint32 gss_display_name(OM_uint32 *minor_status,
                           const gss_name_t input_name,
                           gss_buffer_t output_name_buffer,
                           gss_OID *output_name_type);
OM_uint32 gss_display_status(OM_uint32 *minor_status,
                             OM_uint32 status_value,
                             int status_type,
                             const gss_OID mech_type,
                             OM_uint32 *message_context,
                             gss_buffer_t status_string);
OM_uint32 gss_init_sec_context(OM_uint32 *minor_status,
                               const gss_cred_id_t initiator_cred_handle,
                               gss_ctx_id_t *context_handle,
                               const gss_name_t target_name,
                               const gss_OID mech_type,
                               OM_uint32 req_flags,
                               OM_uint32 time_req,
                               const gss_channel_bindings_t input_chan_bindings,
                               const gss_buffer_t input_token,
                               gss_OID *actual_mech_type,
                               gss_buffer_t output_token,
                               OM_uint32 *ret_flags,
                               OM_uint32 *time_rec);
OM_uint32 gss_wrap_size_limit(OM_uint32 *minor_status,
                              const gss_ctx_id_t context_handle,
                              int conf_req_flag,
                              gss_qop_t qop_req,
                              OM_uint32 req_output_size,
                              OM_uint32 *max_input_size);
OM_uint32 gss_delete_sec_context(OM_uint32 *minor_status,
                                 gss_ctx_id_t *context_handle,
                                 gss_buffer_t output_token);
OM_uint32 gss_inquire_context(OM_uint32 *minor_status,
                              const gss_ctx_id_t context_handle,
                              gss_name_t *src_name,
                              gss_name_t *targ_name,
                              OM_uint32 *lifetime_rec,
                              gss_OID *mech_type,
                              OM_uint32 *ctx_flags,
                              int *locally_initiated,
                              int *open);

#ifdef __cplusplus
}
#endif

#endif /* __TS_SHIM_GSSAPI_H__ */
