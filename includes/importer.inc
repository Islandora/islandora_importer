<?php

/**
 * @file
 * Importer interface for the heavy-lifting.
 */

interface IslandoraBatchImporterInterface {
  /**
   * Determine if we're ready to run our batch process...
   *
   * ... probably checking values.  Might almost be thought of as validation?
   *
   * @param array $form_state
   *   An array containing the form state.
   *
   * @return bool
   *   A boolean indicating whether or not we have all the info to start the
   *   batch is available.
   */
  public static function readyForBatch(array &$form_state);

  /**
   * Get the required info to pass into the file parameter of the constructor.
   *
   * @param array $form_state
   *   An array containing the form state (very likely resulting from the
   *   submission of the form returned from self::getForm
   *
   * @return mixed
   *   Whatever (serializable) data will be passed to self::$itemClass->getOne()
   */
  public static function getBatchInfo(array &$form_state);
}

/**
 * Batch importer interface/abstract class.
 *
 * Implement the abstract functions (and those in the Import Object/item class)
 * and it should be able to work.
 *
 * @author adam@discoverygarden.ca
 */
abstract class IslandoraBatchImporter implements IslandoraBatchImporterInterface {
  /**
   * A string containing the name of a timer used in the run_batch method.
   * @var string
   */
  protected $timerName = 'islandora_batch_import';

  /**
   * A string containing the name of the class to actually do the heavy
   * lifting.
   * @var string
   */
  protected $itemClass = "IslandoraImportObject";

  /**
   * May be a  bit of a misnomer...  We don't really make any assumptions
   *   about what this holds...  We just pass it as a parameter to
   *   abstract methods...  Irrespective of whether the given implementation
   *   actually uses them.  Probably has to be serializable to work properly
   *   with the batch stuff.
   * @var mixed
   */
  protected $file;

  /**
   * The Fedora collection PID into which items should be imported as members.
   * @var string
   */
  protected $parentPid;

  /**
   * Store the batch context, to avoid passing it to individual methods.
   * @var array
   */
  protected $context;

  /**
   * Constructor.
   *
   * Just store the passed in parameters in members.
   *
   * @param mixed $file
   *   May be a  bit of a misnomer...  We don't really make any assumptions
   *   about what this holds...  We just pass it along to the item class.
   * @param array $context
   *   An array containing the batch context.
   */
  public function __construct(&$file, $parent_pid, &$context) {
    $this->file = &$file;
    $this->parentPid = $parent_pid;
    $this->context =& $context;
  }

  /**
   * Get the form for this importer.
   *
   * Could have a multiple-page form given readyForBatch and the form state.
   */
  public static function getForm(array &$form_state) {
    return array(
      'nothing' => array(
        '#type' => 'item',
        '#value' => t('Warning: this importer does not define a form'),
      ),
    );
  }

  /**
   * Get the number of items to import from $this->file.
   *
   * @return int
   *   The number of items left to process.
   */
  public abstract function getNumber();

  /**
   * Get an identifier.
   *
   * @return string
   *   An identifier for the repo.
   */
  protected function getIdentifier(IslandoraTuque $tuque, $namespace) {
    if (empty($this->context['results']['pid_cache'])) {
      // Get enough PIDs for half of the remaining items.
      // (plus one, so we'll always get at least one).
      $this->context['results']['pid_cache'] = (array) $tuque->api->m->getNextPid(
        $namespace,
        intval((($this->context['sandbox']['max'] - $this->context['sandbox']['progress']) / 2) + 1)
      );
    }

    return array_shift($this->context['results']['pid_cache']);
  }

  /**
   * Loop import until we have used up 1/3 of the max execution time.
   */
  public function runBatch() {
    timer_start($this->timerName);

    if (empty($this->context['sandbox'])) {
      $this->context['sandbox']['progress'] = 0;
      $this->context['sandbox']['max'] = $this->getNumber();
    }
    $initial_progress = $this->context['sandbox']['progress'];

    $item_class = $this->itemClass;

    $tuque = new IslandoraTuque();

    // Assume we'll have enough time if less than 1/3 of the execution time has
    // passed. If it's zero, we should have an infinite amount of time (running
    // the batch from drush?). Also, force us to try at least once, to avoid
    // getting stuck in the batch loop forever.
    $max_exec_time = intval(ini_get('max_execution_time')) * 1000;
    while ((
        $max_exec_time === 0
        ||
        ($max_exec_time > 0 && timer_read($this->timerName) < ($max_exec_time / 3))
        || (
          $this->context['sandbox']['progress'] - $initial_progress == 0
        ))
      && (
        $this->context['sandbox']['progress'] < $this->context['sandbox']['max']
      )) {

      // Extract an item from the source info.
      $item = $item_class::getOne($this->file);

      $result = NULL;
      if ($item) {
        $pid = $this->getIdentifier($tuque, $item->pidNamespace);

        $results = $item->writeToFedora($pid, $this->parentPid, $this->context['sandbox']);

        $this->context['results'] = array_merge($this->context['results'], $results);
      }
      else {
        // $item::getOne didn't return anything.
        $this->context['results'][] = array(
          t('Failed to extract information for import item number %num.'),
          array(
            '%num' => $this->context['sandbox']['progress'],
          ),
          WATCHDOG_ERROR,
        );
      }

      $this->context['sandbox']['progress']++;
    }

    if ($this->context['sandbox']['max'] > 0) {
      if ($this->context['sandbox']['progress'] < $this->context['sandbox']['max']) {
        // Treat as floating point to give feedback.
        $this->context['finished'] = 1.0 * $this->context['sandbox']['progress'] / $this->context['sandbox']['max'];
      }
      else {
        // Treat as integers to avoid floating point issues (1 -> finished).
        $this->context['finished'] = $this->context['sandbox']['progress'] / $this->context['sandbox']['max'];
      }
    }

    timer_stop($this->timerName);
  }
}

interface IslandoraImportObjectInterface {
  /**
   * Get an item from the source.
   *
   * Extract one item from the source (modifying the source such that in no
   * longer contains the one being extracted).  Return an instantiation of
   * this class representing the one extracted from the source.
   *
   * Return NULL if there's none to get.
   */
  public static function getOne(&$source);
}

/**
 * A base object (MODS + DC) which can write itself out to Fedora.
 *
 * Might be a good idea to make use of static caching (using static variable)
 * within getter methods.
 *
 * @author adam@discoverygarden.ca
 */
abstract class IslandoraImportObject implements IslandoraImportObjectInterface {
  public $pidNamespace = 'ir';
  public $pidCacheSize = 2;

  /**
   * Allow subclasses to use different content models.
   * @var string
   */
  protected $contentModel = 'ir:citationCModel';

  /**
   * Used to store the source data in the default implementation.
   * @var mixed
   */
  protected $source;

  /**
   * Used for lazy-loading storage.
   * @var string|null
   */
  protected $dc = NULL;
  protected $title = NULL;

  /**
   * Private constructor.
   *
   * Instantiations should be obtained via the "getOne" static method, which
   * should know how to extract and feed what is needed to the constructor.
   *
   * Default implementation just saves the source.
   */
  protected function __construct($source) {
    $this->source = $source;
  }

  /**
   * Get the source text.
   */
  public function getSource() {
    return $this->source;
  }

  /**
   * Get the title.
   *
   * By default, should just get the first titleInfo/title out of the generated
   * MODS.
   *
   * @return string
   *   A string containing the title (eventually used as a label).
   */
  public function getTitle() {
    if ($this->title === NULL) {
      $mods = $this->getMODS();
      if ($mods) {
        $mods_doc = new DOMDocument();
        $mods_doc->loadXML($mods);
        $mods_xpath = new DOMXPath($mods_doc);
        $mods_xpath->registerNamespace('m', 'http://www.loc.gov/mods/v3');

        $this->title = $mods_xpath->evaluate('string(//m:mods/m:titleInfo/m:title/text())');
      }
    }

    return $this->title;
  }

  /**
   * Create some MODS representing us.
   *
   * Don't really care how it is obtained, as long as it's a string containing
   * MODS XML.  Statically caching your result may be a good idea, to avoid
   * regenerating it.
   *
   * @return string
   *   A string containing MODS XML.
   */
  public abstract function getMODS();

  /**
   * Create some DC representing us.
   *
   * Default implementation transforms MODS.
   *
   * @return string
   *   A string containing DC XML.
   */
  public function getDC() {
    if ($this->dc === NULL) {
      $new_dc = static::runXSLTransform(array(
                  'xsl' => drupal_get_path('module', 'islandora_importer') . '/xsl/mods_to_dc.xsl',
                  'input' => $this->getMODS(),
                ));
      if ($new_dc) {
        $this->dc = $new_dc;
      }
    }

    return $this->dc;
  }

  /**
   * Acquire base datastreams structure.
   *
   * Default implementation gets MODS and DC as inline-XML datastreams.
   *
   * @param array $errors
   *   A reference to an array of arrays describing errors which may occur,
   *   used to generate a watchdog entries and a simple HTML log which can be
   *   downloaded after the batch process completes.  Each contained array can
   *   contain four elements:
   *   - A translated string
   * @param array $files
   *   A reference to an array of Drupal file objects, which will be deleted
   *   after the ingest process completes (so temporary files can be dealt
   *   with). (Just seems to need ->uri?)
   *
   * @return array
   *   The array of datastreams as used by
   *   islandora_prepare_new_object().
   */
  protected function getDatastreams(&$errors = NULL, &$files = NULL) {
    $datastreams = array();

    // Ingest the MODS.
    $obtained_mods = $this->getMODS();
    if ($obtained_mods) {
      $name = file_create_filename('MODS record.xml', 'public://');
      $files[] = $file = file_save_data($obtained_mods, $name);
      $datastreams[] = array(
        'dsid' => 'MODS',
        'label' => 'MODS Record',
        'mimetype' => 'text/xml',
        'control_group' => 'X',
        'datastream_file' => file_create_url($file->uri),
      );
    }
    else {
      $errors[] = array(
        t('Failed to produce MODS record for @pid.'),
      );
    }

    // Ingest the DC second.
    $obtained_dc = $this->getDC();
    if ($obtained_dc) {
      $name = file_create_filename('DC record.xml', 'public://');
      $files[] = $file = file_save_data($obtained_dc, $name);
      $datastreams[] = array(
        'dsid' => 'DC',
        'label' => 'DC Record',
        'mimetype' => 'text/xml',
        'control_group' => 'X',
        'datastream_file' => file_create_url($file->uri),
      );
    }
    else {
      $errors[] = array(
        t('Failed to produce DC record for @pid.'),
      );
    }

    return $datastreams;
  }

  /**
   * Write this item to Fedora.
   *
   * @param string $pid
   *   A PID to we can use to which to write our object.
   * @param string $parent_pid
   *   The parent to whom we should relate our object.
   * @param array $sandbox
   *   A reference to the batch $context['sandbox'], so we can pass information
   *   around a little more easily (e.g. for reporting).
   *
   * @return array
   *   An array of arrays, each with up to four values:
   *   - A string containing a message which will be passed to format_string()
   *     and in the watchdog.
   *   - An associative array of placeholders, as used by format_string() and
   *     watchdog, to use in the message.
   *   - An integer representing a watchdog error level
   *   - A string containing a rendered link.
   */
  public function writeToFedora($pid, $parent_pid, &$sandbox) {
    // An array of arrays, containing a structure describing error
    // conditions.
    // - A translated string
    // - An associative array of replacements for the string.
    // - The error level in the watchdog log.
    // - A link to the object in which the error occurred.
    $to_return = array();
    // An array of Drupal file objects to clean up after ingest.
    $files = array();

    $label = $this->getTitle();
    $datastreams = $this->getDatastreams($to_return, $files);
    $relationships = array();

    $content_models = array_unique((array) $this->contentModel);

    $collection_pid = $parent_pid;
    $relationships[] = array(
      'relationship' => 'isMemberOfCollection',
      'pid' => $parent_pid,
    );

    module_load_include('inc', 'islandora', 'includes/utilities');
    $object = islandora_prepare_new_object($pid, $label,
              $datastreams, $content_models, $relationships, $collection_pid);

    $object = islandora_add_object($object);

    foreach ($files as $file) {
      if ($file) {
        file_delete($file);
      }
    }

    if (count($to_return) > 0) {
      foreach ($to_return as &$entry) {
        $entry[] = array(
          '@pid' => $pid,
        );
        $entry[] = WATCHDOG_DEBUG;
        $entry[] = l($pid, "islandora/object/$pid");
      }

      $to_return[] = array(
        t('Error ingesting "%title" as %pid.'),
        array(
          '%pid' => $pid,
          '%title' => $this->getTitle()),
        WATCHDOG_ERROR,
        l($pid, "islandora/object/$pid"),
      );
    }
    else {
      $to_return[] = array(
        t('Ingested "%title" as %pid.'),
        array(
          '%title' => $this->getTitle(),
          '%pid' => $pid,
        ),
        WATCHDOG_INFO,
        l($pid, "islandora/object/$pid"),
      );
    }

    return $to_return;
  }

  /**
   * Run an XSLT, and return the results.
   *
   * @param array $info
   *   An associative array of parameters, containing:
   *   - input: The input XML in a string.
   *   - xsl: The path to an XSLT file.
   *
   * @return string
   *   The transformed XML, as a string.
   */
  protected static function runXSLTransform($info) {
    $xsl = new DOMDocument();
    $xsl->load($info['xsl']);

    $input = new DOMDocument();
    $input->loadXML($info['input']);

    $processor = new XSLTProcessor();
    $processor->importStylesheet($xsl);

    // XXX: Suppressing warnings regarding unregistered prefixes.
    return $processor->transformToXML($input);
  }
}
