/**
 * @file Singleton.h
 * @brief 싱글톤 정의 헤더 파일
*/
#ifndef __SINGLETON_H__
#define __SINGLETON_H__

#define DECLARE_SINGLETON(class_type) \
public: \
	static class_type*  Instance(); \
	static void         RemoveInstance(); \
protected: \
	static class_type*  s_pInstance; \

#define IMPLEMENT_SINGLETON(class_type) \
class_type* class_type::s_pInstance = nullptr; \
class_type* class_type::Instance() \
{ \
	if (!s_pInstance) \
		s_pInstance = new class_type; \
	return s_pInstance; \
} \
void        class_type::RemoveInstance() \
{ \
	if (s_pInstance) \
	{ \
		delete s_pInstance; \
		s_pInstance =  nullptr; \
	} \
}

#define DECLARE_INSTANCE(class_type) \
class_type* Get ## class_type ## ();

#define IMPLEMENT_INSTANCE(class_type) \
class_type* Get ## class_type ## () \
{ \
	static class_type _the_ ## class_type ##; \
    return &_the_ ## class_type ##; \
}

#endif //__SINGLETON_H__
