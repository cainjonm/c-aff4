/*
Copyright 2014 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License.  You may obtain a copy of the
License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.  See the License for the
specific language governing permissions and limitations under the License.
*/

#ifndef  _AFF4_DATA_STORE_H_
#define  _AFF4_DATA_STORE_H_

#include "aff4_base.h"
#include <glog/logging.h>

/**
 * AFF4 objects returned from a resolver remain owned by the resolver. When the
 * caller no longer uses them, they will be returned to the resolver. This
 * scoped ptr retains the ownership of the object. When the AFF4ScopedPtr goes
 * out of scope the object will be returned to the resolver.
 *
 * @param p: An AFF4 object
 * @param resolver: A reference to the resolver. Note that this resolver is
 *   assumed to outlive the AFF4ScopedPtr itself.
 */
template<typename AFF4ObjectType>
class AFF4ScopedPtr {
 protected:
  AFF4ObjectType *ptr_;
  DataStore *resolver_;

 public:
  explicit AFF4ScopedPtr(): ptr_(0) {};
  explicit AFF4ScopedPtr(AFF4ObjectType *p, DataStore *resolver):
      ptr_(p), resolver_(resolver) {
    CHECK(resolver != NULL);
  };

  ~AFF4ScopedPtr() {
  }

  template<class AFF4ObjectOtherType>
  AFF4ScopedPtr<AFF4ObjectOtherType> cast() {
    return AFF4ScopedPtr<AFF4ObjectOtherType>(release(), resolver_);
  };

  AFF4ObjectType *operator->() const {
    CHECK(ptr_ != NULL);
    return ptr_;
  }

  bool operator!(void)  {
    return ptr_ ? false : true;
  }

  AFF4ObjectType& operator*()  {
    return *ptr_;
  }

  AFF4ObjectType *get() const {
    return ptr_;
  }

  AFF4ObjectType *release() {
    AFF4ObjectType* ret = ptr_;
    ptr_ = NULL;
    return ret;
  }

  void reset(AFF4ObjectType *p) {
    ptr_ = p;
  }

  AFF4ScopedPtr(AFF4ScopedPtr&& other) {
    ptr_ = other.release();
    resolver_ = other.resolver_;
  }

  AFF4ScopedPtr(const AFF4ScopedPtr& other) = delete;
  void operator=(const AFF4ScopedPtr& other) = delete;
};


/**
 * @file   data_store.h
 * @author scudette <scudette@google.com>
 * @date   Fri Jan 23 12:11:05 2015
 *
 * @brief This file defines the AFF4 data store abstraction.
 *
 * AFF4 relies on the data store to maintain relational information about the
 * AFF4 universe. This relation information is used to reconstruct objects which
 * have been previously stored in this data store.
 *
 * Note: In this implementation the data store caches all AFF4 objects which
 * have been produced and flushes them when the DataStore::Flush() method is
 * called. The Flush() method is also called during object destruction.

 * This essentially defines a transaction, for example, to open an AFF4 Zip
 * volume, add a new image to it and close it:

~~~~~~~~~~~~~{.c}
  // This essentially starts a transaction in the Volume
  unique_ptr<DataStore> resolver(new MemoryDataStore());

  // This will open and reparse the zip file, populating the resolver.
  AFF4ScopedPtr<AFF4Volume> zip = ZipFile::NewZipFile(resolver.get(), "file.zip");

  // This creates a new image with URN "image.dd" inside the zip file's URN.
  AFF4ScopedPtr<AFF4Image> image = AFF4Image::NewAFF4Image(
       resolver.get(), "image.dd", zip->urn);

  // Write something on the image.
  image->sprintf("Hello world!");

  // This will flush all images, close the zip file etc. This method is also
  // automatically called when the resolver is destructed so it is unnecessary
  // here.
  resolver->Flush();
~~~~~~~~~~~~~

 */

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>
#include <fstream>
#include "aff4_utils.h"
#include <string.h>
#include "rdf.h"

using std::string;
using std::unique_ptr;
using std::unordered_map;
using std::ofstream;
using std::ifstream;
using std::unordered_set;

// Forward declerations for basic AFF4 types.
class AFF4Object;
class AFF4Stream;
class AFF4Volume;


// AFF4_Attributes are a collection of RDFValue objects, keyed by attributes.
typedef unordered_map<string, unique_ptr<RDFValue> > AFF4_Attributes;


/** The abstract data store.

    Data stores know how to serialize RDF statements of the type:

    subject predicate value

    Where both subject and predicate are a URN into the AFF4 space, and value is
    a serialized RDFValue.
*/
class DataStore {
 public:
  DataStore();

  virtual void Set(const URN &urn, const URN &attribute,
                   RDFValue *value) = 0;

  virtual AFF4Status Get(const URN &urn, const URN &attribute,
                         RDFValue &value) = 0;

  virtual void Set(const URN &urn, const URN &attribute,
                   unique_ptr<RDFValue> value) = 0;

  virtual AFF4Status DeleteSubject(const URN &urn) = 0;

  // Dump ourselves to a yaml file.
  virtual AFF4Status DumpToYaml(AFF4Stream &output) = 0;
  virtual AFF4Status DumpToTurtle(AFF4Stream &output) = 0;

  virtual AFF4Status LoadFromYaml(AFF4Stream &output) = 0;
  virtual AFF4Status LoadFromTurtle(AFF4Stream &output) = 0;

  /**
   * Clear all data.
   *
   *
   * @return Status
   */
  virtual AFF4Status Clear() = 0;


  /**
   * Flush all objects cached in the data store.
   *
   *
   * @return Status.
   */
  virtual AFF4Status Flush() = 0;

  /**
   * An object cache for objects created via the AFF4FactoryOpen()
   * interface. Note that the cache owns all objects at all times.
   *
   */
  unordered_map<string, unique_ptr<AFF4Object> > ObjectCache;

  virtual ~DataStore();

  /**
   * Prints out the contents of the resolver to STDOUT. Used for debugging.
   *
   */
  void Dump();


  unordered_set<string> suppressed_rdftypes; /**< These types will not be dumped
                                              * to turtle files. */

  /**
     This is the main entry point into the AFF4 library. Callers use this factory
     method to instantiate an AFF4Object of a particular type based on its
     URN. The factory is passed a resolver which contains the AFF4 RDF metadata
     about the subset of the AFF4 universe we are dealing with.

     The object returned is of the type specified in the resolver (or its base
     type). Callers to the factory must declare their expected types in the template
     arg. If the object is not of the required type, the factory will not
     instantiate it.

     Note that all objects instantiated by the factory are owned by the factory at
     all times. Callers just receive a reference to the object. This allows the
     resolver to maintain a cache of objects and reuse them. Typically, therefore,
     callers may not hold the returned objects for long periods of time. Instead,
     callers should record the URN and use it to retrieve the object in future.

     When the resolver is destroyed, the objects cached by it are flushed. Therefore
     callers may use the lifetime of the resolver as a transaction for created AFF4
     objects.

     The following is an example of how to create an AFF4Image instance and write to
     it:

     ~~~~~~~~~~~{.c}
     void test_ZipFileCreate() {
     unique_ptr<DataStore> resolver(new MemoryDataStore());
     AFF4ScopedPtr<AFF4Stream> file = resolver->AFF4FactoryOpen<AFF4Stream>(
       "test.zip");

     // The backing file is given to the zip.
     AFF4ScopedPtr<AFF4Volume> zip = ZipFile::NewZipFile(resolver.get(), file->urn);

     AFF4ScopedPtr<AFF4Stream> segment = zip->CreateMember("Foobar.txt");
     segment->Write("I am a segment!");
     };
     ~~~~~~~~~~~

     * @param resolver: The resolver to use.
     * @param urn: The URN to instantiate.
     *
     * @return A instance of T or NULL if an object of this type is not known at the
     *         specified URN. Note that callers do not own the object and must not
     *         hold persistent references to it.
     */
  template<typename T>
  AFF4ScopedPtr<T> AFF4FactoryOpen(const URN &urn) {
    // Search the object cache first.
    auto it = ObjectCache.find(urn.value);
    if (it != ObjectCache.end()) {
      it->second->Prepare();
      return AFF4ScopedPtr<T>(dynamic_cast<T *>(it->second.get()), this);
    };

    URN type_urn;
    unique_ptr<AFF4Object> obj;

    // Check if there is a resolver triple for it.
    if (Get(urn, AFF4_TYPE, type_urn) == STATUS_OK) {
      obj = GetAFF4ClassFactory()->CreateInstance(type_urn.value, this);

    } else {
      const uri_components components = urn.Parse();

      // Try to instantiate the handler based on the URN scheme alone.
      obj = GetAFF4ClassFactory()->CreateInstance(components.scheme, this);
    };

    // Failed to find the object.
    if (!obj)
      return AFF4ScopedPtr<T>();

    // Have the object load and initialize itself.
    obj->urn = urn;
    if(obj->LoadFromURN() != STATUS_OK) {
      LOG(WARNING) << "Failed to load " << urn.value.c_str() << " as " <<
          type_urn.value.c_str();

      return AFF4ScopedPtr<T>();
    };

    // Cache the object for next time.
    T *result = dynamic_cast<T *>(obj.get());

    ObjectCache[urn.value] = std::move(obj);

    return AFF4ScopedPtr<T>(result, this);
  };


};


/** A purely in memory data store.

    This data store can be initialized and persisted into a Yaml file.
*/
class MemoryDataStore: public DataStore {

 private:
  // Store a collection of AFF4_Attributes at each URN.
  unordered_map<string, AFF4_Attributes> store;

 public:
  /**
   * Set the RDFValue in the data store. Note that the data store will retain
   * ownership of the value, and therefore callers may not use it after this
   * call.
   *
   * @param urn: The subject to set the attribute for.
   * @param attribute: The attribute to set.
   * @param value: The value.
   */
  virtual void Set(const URN &urn, const URN &attribute, RDFValue *value);
  virtual void Set(const URN &urn, const URN &attribute, unique_ptr<RDFValue> value);

  AFF4Status Get(const URN &urn, const URN &attribute, RDFValue &value);

  virtual AFF4Status DeleteSubject(const URN &urn);

  virtual AFF4Status DumpToYaml(AFF4Stream &output);
  virtual AFF4Status DumpToTurtle(AFF4Stream &output);

  virtual AFF4Status LoadFromYaml(AFF4Stream &output);
  virtual AFF4Status LoadFromTurtle(AFF4Stream &output);

  virtual AFF4Status Clear();
  virtual AFF4Status Flush();

  virtual ~MemoryDataStore();
};

#endif //  _AFF4_DATA_STORE_H_
