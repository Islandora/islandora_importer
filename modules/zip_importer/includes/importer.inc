<?php

/**
 * @file
 * Defines the ZipBatchImporter.
 */

/**
 * Zip file importer.
 * @author adam
 */
class ZipBatchImporter extends IslandoraBatchImporter {

  protected $itemClass = 'ZipBatchImportObject';

  /**
   * Get the number of items to import from $this->file.
   *
   * @see IslandoraBatchImporter::getNumber()
   */
  public function getNumber() {
    return count($this->file['object_info']);
  }

  /**
   * Get the form for this importer.
   *
   * @see IslandoraBatchImporter::getForm()
   */
  public static function getForm(array &$form_state) {
    // Get the list of valid content models.
    $parent = islandora_object_load($form_state['storage']['parent_pid']);

    $cm_options = array();
    $ns_options = array(
      NULL => t('Default namespace'),
    );
    if (isset($parent['COLLECTION_POLICY'])) {
      $policy_stream = $parent['COLLECTION_POLICY'];
      $policy = new CollectionPolicy($policy_stream->content);

      $cm_options = array();
      $ns_options = array(NULL => t('Default namespace'));
      foreach ($policy->getContentModels() as $pid => $info) {
        $cm_options[$pid] = array('title' => $info['name']);

        $cm_namespace = $info['namespace'];
        if (!empty($cm_namespace)) {
          $cm_namespace = $info['namespace'];
          $cm_namespace = explode(':', $info['namespace']);
          $namespace = trim($cm_namespace[0]);
          $ns_options[$namespace] = $namespace;
        }
      }
    }

    return array(
      'fs' => array(
        '#type' => 'fieldset',
        '#title' => t('Zip Batch Importer'),
        '#description' => t('Select the file containing the assets and metadata to import. Assets and metadata will be matched together based on the portion of the filename without the extension - e.g. my_file.xml and my_file.pdf would be combined into a single object.'),
        'file' => array(
          '#type' => 'managed_file',
          '#title' => t('Zip file containing files to import'),
          '#upload_validators' => array(
            'file_validate_extensions' => array('zip'),
          ),
        ),
        'fs' => array(
          '#type' => 'fieldset',
          '#title' => t('Content Model'),
          '#description' => t('The content model(s) to assign to the imported objects.'),
          'content_model' => array(
            '#type' => 'tableselect',
            '#header' => array('title' => t('Name')),
            '#multiple' => TRUE,
            '#options' => $cm_options,
          ),
        ),
        'namespace' => array(
          '#type' => 'select',
          '#title' => t('Object Namespace'),
          '#description' => t('The namespace in which the imported objects will be created.'),
          '#options' => $ns_options,
        ),
        'submit' => array(
          '#type' => 'submit',
          '#value' => t('Import'),
        ),
      ),
    );
  }

  /**
   * Determine if we're ready to run our batch process.
   *
   * @see IslandoraBatchImporter::readyForBatch()
   */
  public static function readyForBatch(array &$form_state) {
    if (empty($form_state['values']['file'])) {
      form_set_error('file', t('Need a Zip file!'));
    }

    $content_models = array_filter($form_state['values']['content_model']);
    if (empty($content_models)) {
      form_set_error('content_model', t('Need to select at least one content model.'));
    }

    return !empty($form_state['values']['file']) && !empty($content_models);
  }

  /**
   * Get the required info to pass into the file parameter of the constructor.
   *
   * @see IslandoraBatchImporter::getBatchInfo()
   */
  public static function getBatchInfo(array &$form_state) {
    // Load the file.
    $file = file_load($form_state['values']['file']);

    // Grab the content models.
    // Cast to an array, in case there are none?
    $content_models = array_filter((array) $form_state['values']['content_model']);

    $info = array(
      'file' => $file,
      'pid_namespace' => $form_state['values']['namespace'],
      'content_model' => $content_models,
      'object_info' => static::getIngestInfo($file),
      'processed_objects' => array(),
    );
    return $info;
  }

  /**
   * Get ingest info.
   *
   * @return array
   *   The ingest info.
   */
  protected static function getIngestInfo($file) {
    $zip = new ZipArchive();
    if (($error = $zip->open(drupal_realpath($file->uri))) !== TRUE) {
      drupal_set_message(t('Error opening the provided Zip file.  Code: %code', array(
            '%code' => $error,
          )));
      return;
    }

    $count = $zip->numFiles;

    $info = array();
    for ($i = 0; $i < $count; $i++) {
      $name = $zip->getNameIndex($i);
      $path_info = pathinfo($name);

      $object_key = $path_info['dirname'] . '-_-' . $path_info['filename'];
      if (strpos($path_info['dirname'], '__MACOSX') === FALSE && isset($path_info['extension'])) {
        $datastream_key = strtolower($path_info['extension']);
        $info[$object_key][$datastream_key] = $name;
      }
    }

    $zip->close();

    return $info;
  }

}

/**
 * ZIP import object.
 *
 * Actually does the heavy-lifting during the import.
 * @author adam
 */
class ZipBatchImportObject extends IslandoraImportObject {

  protected $mods;

  /**
   * Constructor.
   */
  protected function __construct($source) {
    parent::__construct($source);
    $this->pidNamespace = $this->source['pid_namespace'];
    $this->contentModel = (array) $this->source['content_model'];
  }

  /**
   * Get an item from the source.
   *
   * @see IslandoraImportObject::getOne()
   */
  public static function getOne(&$info) {
    $record = array(
      'pid_namespace' => $info['pid_namespace'],
      'file' => $info['file'],
      'content_model' => $info['content_model'],
    );
    $key = key($info['object_info']);
    $record['object_info'] = $info['processed_objects'][$key] = $info['object_info'][$key];
    unset($info['object_info'][$key]);
    return (empty($record) ? FALSE : new static($record));
  }

  /**
   * Determine the datastream ID for the given file.
   *
   * @param string $name
   *   A filename for which to determine a mimetype.
   *
   * @return array
   *   An array containing two values:
   *   - Either boolean FALSE or the datastream ID, as mapped in the
   *     DS-COMPOSITE-MODELs of the selected object.
   *   - A string containing the detected mimetype of the file.
   */
  protected function determineDSIDAndMimetype($name) {
    // Something like this would be nice to have...  Need a way to indicate
    // primary assets, though...  Think about basic image, where the content
    // model can contain "OBJ" image/jpeg, "TN" image/jpeg and so on...
    static $mime_detect = new MimeDetect();
    static $dsids = NULL;

    if ($dsids === NULL) {
      module_load_include('inc', 'islandora', 'includes/utilities');
      $models = (array) $this->source['content_model'];
      $dsids = islandora_get_datastreams_requirements_from_models($models);
    }

    $mimetype = $mime_detect->getMimetype($name);

    $dsid = FALSE;
    // Determine which stream this should be...  Uses the first matching, as
    // received from the DC-COMPOSITE.
    foreach ($dsids as $ds => $info) {
      if (in_array($mimetype, $info['mime'])) {
        $dsid = $ds;
        break;
      }
    }

    return array($dsid, $mimetype);
  }

  /**
   * Acquire base datastreams structure.
   *
   * @see IslandoraImportObject::getDatastreams()
   */
  protected function getDatastreams(&$errors = NULL, &$files = NULL) {
    module_load_include('inc', 'islandora', 'includes/utilities');
    $to_return = parent::getDatastreams($errors, $files);

    foreach ($this->source['object_info'] as $datastream => $name) {
      if ($datastream == 'xml') {
        // XML streams are handled via the parent implementation,
        // (via get_{mods,dc}()) so let's go to the next item.
        continue;
      }

      // Determine which stream this should be...  Uses the first matching, as
      // as received from the DC-COMPOSITE.
      list($dsid, $mimetype) = $this->determineDSIDAndMimetype($name);

      if (!$dsid) {
        drupal_set_message(t('The detected mimetype of %filename (@mime) is not supported by any of the selected content models.', array(
          '@mime' => $mimetype,
          '%filename' => $name,
        )), 'warning', FALSE);
        continue;
      }

      $zip = new ZipArchive();
      $zip->open(drupal_realpath($this->source['file']->uri));
      if ($zip->extractTo(drupal_realpath('public://'), $name)) {
        $file = new stdClass();
        $file->uri = "public://$name";
        $files[] = $file;

        $to_return += array(
          $dsid => array(
            'dsid' => $dsid,
            'label' => "$dsid datastream",
            'mimetype' => $mimetype,
            'datastream_file' => file_create_url($file->uri),
            'control_group' => 'M',
          ),
        );
      }
      $zip->close();
    }

    return $to_return;
  }

  /**
   * Generates a MODS document repersenting the imported data.
   *
   * @see IslandoraImportObject::getMODS()
   */
  public function getMODS() {
    if ($this->mods === NULL) {
      // If we have an XML stream which is:
      if (isset($this->source['object_info']['xml'])) {
        $zip = new ZipArchive();
        $zip->open(drupal_realpath($this->source['file']->uri));
        $xml = $zip->getFromName($this->source['object_info']['xml']);
        $zip->close();

        if ($xml) {
          $s_xml = new SimpleXMLElement($xml);
          // MODS, set.
          if ($s_xml->getName() == 'mods') {
            $this->mods = $xml;
          }
          // MARCXML, transform to MODS and set.
          elseif ($s_xml->getName() == 'record') {
            $this->mods = static::runXSLTransform(array(
                  'input' => $xml,
                  'xsl' => drupal_get_path('module', 'zip_importer') . '/xsl/MARC21slim2MODS3-4.xsl',
                ));
          }
          // DC, transform to MODS and set.
          elseif ($s_xml->getName() == 'dc') {
            $this->mods = static::runXSLTransform(array(
                  'input' => $xml,
                  'xsl' => drupal_get_path('module', 'zip_importer') . '/xsl/simpleDC2MODS.xsl',
                ));
          }
          // Unrecognized format.
          else {
            // Do nothing?
          }
        }
      }

      if (empty($this->mods)) {
        // If we do not yet have any XML, create a dumb MODS with just the
        // title, and set it.
        $title = pathinfo(reset($this->source['object_info']), PATHINFO_FILENAME);

        $this->mods = <<<EOXML
<mods xmlns:mods="http://www.loc.gov/mods/v3" xmlns="http://www.loc.gov/mods/v3">
  <titleInfo>
    <title>$title</title>
  </titleInfo>
</mods>
EOXML;
      }
    }

    return $this->mods;
  }

  /**
   * Generate DC to describe the imported data.
   *
   * @see ZipBatchImportObject::getDC()
   *
   * @return string
   *   A string containing DC XML.
   */
  public function getDC() {
    if ($this->dc === NULL) {
      // If we have an XML stream which is DC, set it.
      if (isset($this->source['object_info']['xml'])) {
        $zip = new ZipArchive();
        $zip->open(drupal_realpath($this->source['file']->uri));
        $xml = $zip->getFromName($this->source['object_info']['xml']);
        $zip->close();

        if ($xml) {
          $s_xml = new SimpleXMLElement($xml);
          if ($s_xml->getName() == 'dc') {
            $this->dc = $xml;
          }
        }
      }
      // Otherwise, call the parent implementation (transform from MODS).
      if (empty($this->dc)) {
        parent::getDC();
      }
    }

    return $this->dc;
  }
}
