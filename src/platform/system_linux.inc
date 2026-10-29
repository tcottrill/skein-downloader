// wchar_t is UTF-32 on Linux. Strict like the Windows converters: overlong forms,
// surrogates, values past U+10FFFF and truncated sequences are rejected.
static bool scalar(uint32_t c){return c<=0x10FFFF&&(c<0xD800||c>0xDFFF);}
std::wstring wide(const std::string& value){
    std::wstring out;out.reserve(value.size());
    for(size_t i=0;i<value.size();){
        auto lead=(unsigned char)value[i];size_t length=lead<0x80?1:(lead>>5)==6?2:(lead>>4)==14?3:(lead>>3)==30?4:0;
        if(!length||i+length>value.size())throw std::runtime_error("Invalid UTF-8 text");
        uint32_t c=length==1?lead:lead&(0x7F>>length);
        for(size_t k=1;k<length;++k){auto next=(unsigned char)value[i+k];if((next>>6)!=2)throw std::runtime_error("Invalid UTF-8 text");c=c<<6|(next&0x3F);}
        static const uint32_t minimum[]={0,0,0x80,0x800,0x10000};
        if(c<minimum[length]||!scalar(c))throw std::runtime_error("Invalid UTF-8 text");
        out.push_back((wchar_t)c);i+=length;
    }
    return out;
}
std::string utf8(const std::wstring& value){
    std::string out;out.reserve(value.size());
    for(auto w:value){
        auto c=(uint32_t)w;if(!scalar(c))throw std::runtime_error("Invalid Unicode text");
        if(c<0x80)out+=char(c);
        else if(c<0x800){out+=char(0xC0|c>>6);out+=char(0x80|(c&0x3F));}
        else if(c<0x10000){out+=char(0xE0|c>>12);out+=char(0x80|(c>>6&0x3F));out+=char(0x80|(c&0x3F));}
        else{out+=char(0xF0|c>>18);out+=char(0x80|(c>>12&0x3F));out+=char(0x80|(c>>6&0x3F));out+=char(0x80|(c&0x3F));}
    }
    return out;
}
static std::string hashFile(const Path& path,bool sha256,std::atomic_bool& stop){
    auto context=EVP_MD_CTX_new();if(!context)throw std::runtime_error("Cannot initialize hash");
    struct Cleanup{EVP_MD_CTX* c;~Cleanup(){EVP_MD_CTX_free(c);}} cleanup{context};
    if(EVP_DigestInit_ex(context,sha256?EVP_sha256():EVP_sha1(),nullptr)!=1)throw std::runtime_error(sha256?"Cannot initialize SHA-256":"Cannot initialize SHA-1");
    std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot open file for verification");char buffer[65536];
    while(in){if(stop)throw std::runtime_error("Paused");in.read(buffer,sizeof buffer);if(EVP_DigestUpdate(context,buffer,in.gcount())!=1)throw std::runtime_error("Hash failed");}
    if(!in.eof())throw std::runtime_error("File read failed during verification");
    unsigned char bytes[EVP_MAX_MD_SIZE];unsigned size=0;if(EVP_DigestFinal_ex(context,bytes,&size)!=1)throw std::runtime_error("Hash failed");
    std::ostringstream out;for(unsigned i=0;i<size;++i)out<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)bytes[i];return out.str();
}
std::string sha1File(const Path& path,std::atomic_bool& stop){return hashFile(path,false,stop);}
std::string sha256File(const Path& path,std::atomic_bool& stop){return hashFile(path,true,stop);}
