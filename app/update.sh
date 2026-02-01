#!/bin/sh

# -------------------------------------------------------------------------
# Axis Camera Certificate Deploy Hook for LEGO
# -------------------------------------------------------------------------
# LEGO automatically provides these variables:
# $LEGO_CERT_PATH - Path to the new certificate (fullchain)
# $LEGO_KEY_PATH  - Path to the private key
# -------------------------------------------------------------------------

# Configuration
# Use 'root' and your camera password. 
# Since this runs locally, you might use a specific service account if available.
#API_USER="root"
#API_PASS="pass"

# Generate a unique ID for this certificate (e.g., lego-16788822)
CERT_ID="lego-$(date +%s)"
HOST="127.0.0.1"

RESULT=''
PREFIX='<SOAP-ENV:Envelope
    xmlns:wsdl="http://schemas.xmlsoap.org/wsdl/"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:tds="http://www.onvif.org/ver10/device/wsdl"
    xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
    xmlns:xsd="http://www.w3.org/2001/XMLSchema"
    xmlns:onvif="http://www.onvif.org/ver10/schema"
    xmlns:tt="http://www.onvif.org/ver10/schema"
    xmlns:SOAP-ENV="http://www.w3.org/2003/05/soap-envelope">
    <SOAP-ENV:Body>
        '
POSTFIX='
    </SOAP-ENV:Body>
</SOAP-ENV:Envelope>'
soap() {
    echo "$(curl --silent --request POST --insecure \
      --anyauth \
      --user "${API_USER}:${API_PASS}" \
      --header "Content-Type: application/xml" \
        "http://${HOST}/vapix/services" \
      --data "${PREFIX}$1${POSTFIX}")"
}


OLD_CERT_ID="$(soap '<tds:GetCertificates xmlns="http://www.onvif.org/ver10/device/wsdl" />' \
            | grep -oP '(?<=CertificateID>)lego-[^<]+' | awk '{print "            <CertificateID>" $0 "</CertificateID>"}')"

echo "Deploying new certificate to Axis Camera..."

# 2. Upload the Certificate (REST API)
# We use the modern REST endpoint for certificate management.
RESPONSE="$(soap '<tds:LoadCertificateWithPrivateKey xmlns="http://www.onvif.org/ver10/device/wsdl">
            <CertificateWithPrivateKey>
                <tt:CertificateID>'"${CERT_ID}"'</tt:CertificateID>
                <tt:Certificate>
                    <tt:Data>'"$(cat "${LEGO_CERT_PATH}")"'</tt:Data>
                </tt:Certificate>
                <tt:PrivateKey>
                    <tt:Data>'"$(cat "${LEGO_KEY_PATH}")"'</tt:Data>
                </tt:PrivateKey>
            </CertificateWithPrivateKey>
        </tds:LoadCertificateWithPrivateKey>')"

if echo "${RESPONSE}" | grep -q "error"; then
    echo "Error uploading certificate: ${RESPONSE}"
    exit 1
fi

echo "Certificate uploaded with ID: ${CERT_ID}"

# 3. Activate the Certificate (VAPIX Param API)
# We set the Network.HTTPS.SSLCertificateId parameter to the new ID.
RESPONSE="$(soap '<aweb:SetWebServerTlsConfiguration xmlns="http://www.axis.com/vapix/ws/webserver">
            <Configuration>
                <Tls>true</Tls>
                <aweb:ConnectionPolicies>
                    <aweb:Admin>Https</aweb:Admin>
                    <aweb:Operator>Https</aweb:Operator>
                    <aweb:Viewer>Https</aweb:Viewer>
                </aweb:ConnectionPolicies>
                <aweb:Ciphers>
                    <acert:Cipher>ECDHE-ECDSA-AES128-GCM-SHA256</acert:Cipher>
                    <acert:Cipher>ECDHE-RSA-AES128-GCM-SHA256</acert:Cipher>
                    <acert:Cipher>ECDHE-ECDSA-AES256-GCM-SHA384</acert:Cipher>
                    <acert:Cipher>ECDHE-RSA-AES256-GCM-SHA384</acert:Cipher>
                    <acert:Cipher>ECDHE-ECDSA-CHACHA20-POLY1305</acert:Cipher>
                    <acert:Cipher>ECDHE-RSA-CHACHA20-POLY1305</acert:Cipher>
                    <acert:Cipher>DHE-RSA-AES128-GCM-SHA256</acert:Cipher>
                    <acert:Cipher>DHE-RSA-AES256-GCM-SHA384</acert:Cipher>
                </aweb:Ciphers>
                <aweb:CertificateSet>
                    <acert:Certificates>
                        <acert:Id>'"${CERT_ID}"'</acert:Id>
                    </acert:Certificates>
                    <acert:CACertificates />
                    <acert:TrustedCertificates />
                </aweb:CertificateSet>
            </Configuration>
        </aweb:SetWebServerTlsConfiguration>')"

if echo "${RESPONSE}" | grep -q "OK"; then
    echo "Successfully activated certificate: ${CERT_ID}"
else
    echo "Error activating certificate: ${RESPONSE}"
    exit 1
fi

# 4. (Optional) Cleanup Old Certificates
# You might want to delete certificates older than the current one here
# using the 'remove' action in the API, or manage it manually.
RESPONSE="$(soap '<tds:DeleteCertificates xmlns="http://www.onvif.org/ver10/device/wsdl">
'"${OLD_CERT_ID}"'
        </tds:DeleteCertificates>')"

if echo "${RESPONSE}" | grep -q "OK"; then
    echo "Successfully deleted certificates: $(echo "${OLD_CERT_ID}" | grep -oP '(?<=CertificateID>)[^<]+')"
else
    echo "Error deleting certificate: ${RESPONSE}"
    exit 1
fi
