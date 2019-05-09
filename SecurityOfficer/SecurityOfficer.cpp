#include "SecurityOfficer.h"
#include "SecurityContext.h"

namespace WPEFramework {
namespace Plugin {

    SERVICE_REGISTRATION(SecurityOfficer, 1, 0);

    static Core::ProxyPoolType<Web::TextBody> textFactory(1);

    class SecurityCallsign : public PluginHost::ISubSystem::ISecurity {
    public:
        SecurityCallsign() = delete;
        SecurityCallsign(const SecurityCallsign&) = delete;
        SecurityCallsign& operator=(const SecurityCallsign&) = delete;

        SecurityCallsign(const string callsign)
            : _callsign(callsign)
        {
        }
        virtual ~SecurityCallsign()
        {
        }

    public:
        // Security information
        virtual string Callsign() const
        {
            return (_callsign);
        }

    private:
        BEGIN_INTERFACE_MAP(SecurityCallsign)
        INTERFACE_ENTRY(PluginHost::ISubSystem::ISecurity)
        END_INTERFACE_MAP

    private:
        const string _callsign;
    };

    SecurityOfficer::SecurityOfficer()
    {
        for (uint8_t index = 0; index < sizeof(_secretKey); index++) {
            Crypto::Random(_secretKey[index]);
        }
    }

    /* virtual */ SecurityOfficer::~SecurityOfficer()
    {
    }

    /* virtual */ const string SecurityOfficer::Initialize(PluginHost::IShell* service)
    {
        Config config;
        config.FromString(service->ConfigLine());
        string version = service->Version();

        _skipURL = static_cast<uint8_t>(service->WebPrefix().length());
        Core::File aclFile(service->PersistentPath() + config.ACL.Value(), true);

        if (aclFile.Exists() == false) {
            aclFile = service->DataPath() + config.ACL.Value();
        }
        if ((aclFile.Exists() == true) && (aclFile.Open(true) == true)) {

            if (_acl.Load(aclFile) == Core::ERROR_INCOMPLETE_CONFIG) {
                AccessControlList::Iterator index(_acl.Unreferenced());
                while (index.Next()) {
                    SYSLOG(Logging::Startup, (_T("Role: %s not referenced"), index.Current().c_str()));
                }
                index = _acl.Undefined();
                while (index.Next()) {
                    SYSLOG(Logging::Startup, (_T("Role: %s is undefined"), index.Current().c_str()));
                }
            }
        }

        PluginHost::ISubSystem* subSystem = service->SubSystems();

        ASSERT(subSystem != nullptr);

        if (subSystem != nullptr) {
            Core::Sink<SecurityCallsign> information(service->Callsign());

            if (subSystem->IsActive(PluginHost::ISubSystem::SECURITY) != false) {
                SYSLOG(Logging::Startup, (_T("Security is not defined as External !!")));
            } 

			subSystem->Set(PluginHost::ISubSystem::SECURITY, &information);

            subSystem->Release();
        }

        // On success return empty, to indicate there is no error text.
        return _T("");
    }

    /* virtual */ void SecurityOfficer::Deinitialize(PluginHost::IShell* service)
    {
        PluginHost::ISubSystem* subSystem = service->SubSystems();

        ASSERT(subSystem != nullptr);

        if (subSystem != nullptr) {
            subSystem->Set(PluginHost::ISubSystem::NOT_SECURITY, nullptr);
            subSystem->Release();
        }
        _acl.Clear();
    }

    /* virtual */ string SecurityOfficer::Information() const
    {
        // No additional info to report.
        return (string());
    }

    /* virtual */ uint32_t SecurityOfficer::CreateToken(const uint16_t length, const uint8_t buffer[], string& token)
    {
        // Generate the token from the buffer coming in...
        Web::JSONWebToken newToken(Web::JSONWebToken::SHA256, sizeof(_secretKey), _secretKey);

        return (newToken.Encode(token, length, buffer) > 0 ? Core::ERROR_NONE : Core::ERROR_UNAVAILABLE);
    }

    /* virtual */ PluginHost::ISecurity* SecurityOfficer::Officer(const string& token)
    {
        PluginHost::ISecurity* result = nullptr;

        Web::JSONWebToken webToken(Web::JSONWebToken::SHA256, sizeof(_secretKey), _secretKey);
        uint16_t load = webToken.PayloadLength(token);

        // Validate the token
        if (load != static_cast<uint16_t>(~0)) {
            // It is potentially a valid token, extract the payload.
            uint8_t* payload = reinterpret_cast<uint8_t*>(ALLOCA(load));

            load = webToken.Decode(token, load, payload);

            if (load != static_cast<uint16_t>(~0)) {
                // Seems like we extracted a valid payload, time to create an security context
                result = Core::Service<SecurityContext>::Create<SecurityContext>(&_acl, load, payload);
            }
        }
        return (result);
    }

    /* virtual */ void SecurityOfficer::Inbound(Web::Request& request)
    {
        request.Body(textFactory.Element());
    }

    /* virtual */ Core::ProxyType<Web::Response> SecurityOfficer::Process(const Web::Request& request)
    {
        Core::ProxyType<Web::Response> result(PluginHost::Factories::Instance().Response());
        Core::TextSegmentIterator index(Core::TextFragment(request.Path, _skipURL, static_cast<uint32_t>(request.Path.length() - _skipURL)), false, '/');

        result->ErrorCode = Web::STATUS_BAD_REQUEST;
        result->Message = "Unknown error";

        index.Next();

		if (index.Next() == true) {
            // We might be receiving a plugin download request.
            if ((request.Verb == Web::Request::HTTP_PUT) && (request.HasBody() == true)) {
                if (index.Current() == _T("Token")) {
                    Core::ProxyType<const Web::TextBody> data(request.Body<Web::TextBody>());

                    if (data.IsValid() == true) {
                        Core::ProxyType<Web::TextBody> token = textFactory.Element();
                        const string& byteBag(static_cast<const string&>(*data));
                        uint32_t code = CreateToken(byteBag.length(), reinterpret_cast<const uint8_t*>(byteBag.c_str()), static_cast<string&>(*token));

                        if (code == Core::ERROR_NONE) {

                            result->Body(token);
                            result->ContentType = Web::MIMETypes::MIME_TEXT;
                            result->ErrorCode = Core::ERROR_NONE;
                        }
                    }
                }
            } else if ( (request.Verb == Web::Request::HTTP_GET) && (index.Current() == _T("Valid")) ) {
                result->ErrorCode = Web::STATUS_FORBIDDEN;
                result->Message = _T("Missing token");

                if (request.WebToken.IsSet()) {
                    Web::JSONWebToken webToken(Web::JSONWebToken::SHA256, sizeof(_secretKey), _secretKey);
                    const string& token = request.WebToken.Value().Token();
                    uint16_t load = webToken.PayloadLength(token);

                    // Validate the token
                    if (load != static_cast<uint16_t>(~0)) {
                        // It is potentially a valid token, extract the payload.
                        uint8_t* payload = reinterpret_cast<uint8_t*>(ALLOCA(load));

                        load = webToken.Decode(token, load, payload);

                        if (load == static_cast<uint16_t>(~0)) {
                            result->ErrorCode = Web::STATUS_FORBIDDEN;
                            result->Message = _T("Invalid token");
                        } else {
                            result->ErrorCode = Web::STATUS_OK;
                            result->Message = _T("Valid token");
                            TRACE(Trace::Information, (_T("Token contents: %s"), reinterpret_cast<const TCHAR*>(payload)));
                        }
                    }
                
				}
            }
        }
		return (result);
    }

} // namespace Plugin
} // namespace WPEFramework